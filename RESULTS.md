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
  `docs/biome-vegetation.png`.
- **Bushes (closing the phase).** The same rule at a finer grain: generation's
  3x3 leaf clump grows at a 5-block bush cell's canonical column where the
  ground is grass (grassland and forest) at generation's 0.22 density, and a
  ground-level oak leaf decays where the biome holds no bushes. A canopy's
  lowest leaves sit three blocks up the trunk, so "oak leaves on solid
  ground" identifies a bush without any search. Entries carry bit 29 beside
  the decay bit; the driver relights and rescans a one-layer box. Seed 7 with
  `CF_VEG_BUDGET=8`, sampled every 100 frames over 3,000: 30 bush actions to 6
  tree actions, ~6 ms per bush against ~40 ms per tree, and the idle scan
  stays at 1-2 ms (fps unchanged at budget 0 and 1).

## G37: what `pmap_n` scaling actually costs (2026-09-04)

Machine under heavy external load (load average 100–130) throughout, so every
number below is a ratio between two configurations measured back to back.
Probe: `probes/pmap_scaling/pmap_scaling.march`, one `pmap_n` over 64 tasks,
interchangeable bodies. Runtime built with `-DMARCH_NUM_SCHEDULERS=16`, because
the stock runtime silently caps at 4 (GAPS.md G71).

| body | 1 thread | 14 threads | speedup |
|---|---|---|---|
| A pure arithmetic | 1 597 ms | 140 ms | **11.8x** |
| D shared `NativeU8Arr`, read directly | 476 ms | 95 ms | **5.8x** |
| E private `Array.PVec` | 134 603 ms | 39 482 ms | 3.4x |
| C cons-cell allocation | 63 ms | 185 ms | **0.34x** |
| B shared `Array.PVec` | 108 401 ms | 96 577 ms | 1.12x |
| F shared array behind a one-field wrapper | 2 761 ms | 11 188 ms | **0.25x** |

D against F is the load-bearing comparison: same array, same sharing, same
number of reads, and the only difference is a `match` that projects the array
out of a wrapper. B against E is the same story with the sharing varied instead
of the projection.

Removing the global allocation counter (GAPS.md G72) turns C from 0.34x into
3.4x and makes its 14-thread case 11.6x faster.

Whole-world meshing, same machine, both changes where noted:

| | 1 | 4 | 8 | 14 |
|---|---|---|---|---|
| stock (cap 4: 8 and 14 are really 4) | 628 | 390 | 392 | 393 |
| cap raised to 16 | 620 | 390 | 408 | 434 |
| cap 16 + no global alloc counter | 618 | 335 | 302 | **297** |

Raising the cap alone does nothing for the mesher, because its ceiling is G73,
not the thread count. The remaining gap to the ~44 ms a perfect 14x would give
is the refcount traffic on the shared chunks.

## M7 — escape menu (2026-09-04)

`Esc` opens a menu over the dimmed world with NEW GAME, an editable SEED field
and QUIT. The window, GL context, texture, camera projections and the actor pool
outlive a session; a new game rebuilds the world, the meshes and the player.

### Verification

**The restarted world is the world that seed makes.** `CF_AUTOMENU=<frame>`
opens the menu, types `424242` into the seed field and starts a new game five
frames later:

```
seed 7 (CF_SEED)
  mesh all (greedy sections): 450 ms wall, 231534 vertices (27192 water)
new game, seed 424242
ran 26 frames …
seed 424242 (set CF_SEED to reproduce)
  mesh all (greedy sections): 488 ms wall, 252108 vertices (31044 water)
```

and a *fresh process* at `CF_SEED=424242` produces **252108 vertices (31044
water)** — identical. That is the property that breaks if the session rebuild
misses a piece of state.

**The text says what it claims to say.** Rather than looking at the screenshot,
the dumped frame is sampled at the centre of every 3x5 glyph cell the renderer
should have filled, and the resulting bitmasks are decoded back through the font
table:

```
item 0: rendered = 'NEW GAME'   expected 'NEW GAME'   OK
item 1: rendered = 'QUIT'       expected 'QUIT'       OK
seed label: rendered = 'SEED'
seed field reads: 424242
```

The panel is opaque (80 sampled interior pixels take three adjacent shades of
one colour) and the rest of the screen dims (242,191,140 → 109,86,63 above the
panel).

### Two bugs this shook out, both silent

- **A double-booked VBO slot.** The menu uploaded to 250, which the map-view
  player marker already owns, and drew it through the textured path instead of
  the colour path. Nothing errored: `glGetError` stayed 0 and the debug trace
  showed the upload and the draw of all 1272 vertices. The slot map is now a
  comment in the source, because the shim's slots are one flat array.
- **A dump overwritten by the next session.** `CF_DUMP_FRAME` fires once per
  *session*, so a scripted restart dumped twice to the same path and every pixel
  measurement was really being taken on session two, with the menu closed. The
  verification run now ends before the restart fires. Two rounds of pixel
  analysis were wasted on the wrong image.

### A regression this exposed, which is not the menu's

The frame loop is allocating **173 live objects per frame**, not the 1 recorded
above. Measured at `git stash` on the commit before this work and after it:
identical, 17349 objects over frames 100-200 in both, and unchanged by
`CF_SUN`, `CF_WEATHER` or whether the menu is open. So it predates the menu and
arrived with one of biomes, weather or vegetation. The "1 per frame" figures
earlier in this file are stale for the current build; the number is not
re-measured per feature, which is how it went unnoticed. Not chased here.

**Correction (2026-09-04).** The commit message for this feature says the actors
are reseeded "because March has no Actor.stop". That is wrong: `kill(pid)` and
`is_alive(pid)` are builtins and work. The pool is reused because it is cheaper,
not because it had to be. See GAPS.md G75.

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
~1.5 ms each. `CF_SPRING_DENSITY` is the lever for it. `docs/spring-brook.png` is
seed 5, whose spring sits four columns from spawn: a pool at the mouth and
evaporating tips scattered downslope -- at rate 1 a brook reads as a chain of
puddles rather than a ribbon.

## Spray

Drops and spring mouths. When `give` lands water in a cell with air beneath it,
the actor appends a `kind 4` entry with the cell; `apply_reply` turns it into
three spray particles there. Every spring bubbles two particles every ten
frames from the shim's spring list; placing a spring bursts forty. A second
pool in the shim (`CF_SPRAY`, default 512), white quads fading with life,
drawn through the precipitation path in slot 247. Seed 5 at frame 890, spray
on against off: 500 pixels differ, all around the spring by spawn
(`docs/spring-spray.png`).

Two March traps on the way: `&&` does not short-circuit (G33), so a
`dy > 0 && C.get(.., dy - 1, ..)` guard still read y = -1 once water had fallen
to the bottom of a test world with no floor -- nested `if` now; and the shaft
test's world had a single stone block as its "floor", off which water fell to
y = 0 -- a full layer now.

## Water-only section rebuilds

Every water reply used to rebuild all three passes (opaque, water, foliage) of
each touched section, so a brook that never settles re-meshed its terrain
forever. `CM.rebuild_section_water` rebuilds only the water pass, and the
reply path (`remesh_sections_water` and friends, with the same ±1-section and
neighbour-chunk widening) uses it. Edits, felling, migration and growth still
take the full rebuild, since they change opaque blocks and baked light.

Measured with `CF_WORKERS=1`, seed 5, five springs at rate 1: the remesh
share of a water tick fell from 27-36 ms to 3.5-5 ms; the canal tick
(`CF_AUTOCANAL`) from 25 ms to 2.4 ms.

Staleness check: `CF_REMESH_ALL_AT=<frame>` rebuilds every section of every
chunk (the ground truth) and, for one instrumented run, reported any section
whose rebuilt buffer differed from the stored one. Frame 890 on the brook
world (seed 5) and on a canal world with no springs (finite water that spreads
and dries): **0 pixels differ and 0 sections change** between the water-only
build and the full rebuild.

The check took a detour worth recording. Unpinned, a full rebuild and the
plain run differed by ~5,000 pixels of 1-7 level speckle on lit slopes, and
two plain runs by 400-1,500 -- which looked like stale baked light. The
buffers were bit-identical; the sun was not. The sun angle comes from wall
time, and a rebuild stalls a frame for a few hundred ms, so every "stale
geometry" delta was the sun moving. **Frame comparisons need `CF_SUN` as
well as `CF_TIME`**; with both pinned the brook world is deterministic run to
run, so the earlier note that springs make runs nondeterministic was the
same mistake. Separately, a launch occasionally exits 0 without writing the
dump; the harness now retries.

## The world tick: 55 ms frames to 11 (2026-09-04)

The game ran at ~82 fps with a visible stall, having been vsync-capped at 120.
Bisecting the frame rate over the branch found no single culprit and no
regression in the renderer -- the cost had accumulated across the biome work,
in four steps, and none of it was the features being expensive:

| commit | fps | live objects/frame |
|--------|-----|--------------------|
| `6eb9dca` pre-biome main | 471 | 1 |
| `8ee3a4d` biome field ticked | 200 | 1 |
| `e07cfc8` surface migration | 154 | 72 |
| `8629e17` vegetation | 90 | 173 |
| `0131ae8` springs (start of this work) | 82 | 230 |

The tick, every ten frames, cost **~80 ms**. Not amortised: one frame in ten
carried all of it.

### What it was

Six things, in the order they were found. Each was measured before and after,
and each was verified not to change what the world does (see the oracles
below). The tick's phase timings, `CF_AUTOFLOW` and the frame gauge:

| | before | after |
|---|---|---|
| `Light.sky_floor`, called per tree by `Biome.rescan_box` | 21.5 ms | gone |
| vegetation scan (`&&` does not short-circuit, G33) | 7.9 ms | 0.5 ms |
| biome water rescan, per-column `block_at` | 19 ms | 0.43 ms |
| retexture field update, 64 whole-array copies | 11 ms | 8.8 ms |
| `Biome.tick` ease + reclassify, all 16,384 columns | 4.8 ms | 1.9 ms |
| section rebuilds, all three passes whatever changed | 9-18 ms | queued |

- **`sky_floor` was the whole of `rescan_box`.** It walks every chunk down from
  y=255 -- 2.1 M voxel reads -- to return the constant 127, and `rescan_box`
  called it every time. A box over ONE column cost 21.8 ms; over 169, 24.8. It
  takes the edit's top as a parameter now. The same 21.5 ms came off every tree
  chop, which had been a documented mystery.
- **G33 again.** `Veg.can_decay` is `!wants_trees(b) && trunk_base(...)`, and
  `&&` evaluates both sides, so `trunk_base` walked thirteen deep for all 2048
  columns a tick scans -- every one of them in grassland, where the first test
  had already said no.
- **`W.block_at` re-does the chunk-trie lookup per read.** The biome water pass
  did 32k of them a tick in world coordinates; walked chunk by chunk instead
  (one lookup per 256 columns) it is 45x faster. `Veg.trunk_base` had the same
  shape, 26 lookups a column.
- **The dirty set.** Ease and classify ran over all 16,384 columns to discover
  that almost none had moved. A column is settled when both axes sit exactly on
  target -- `ease` assigns the target itself once within a step, so the test is
  exact -- and the hold counter is clear, which by that counter's own rule means
  the stored biome IS the classification. Only three things can wake one: the
  three writers of the heightmap, and a change in the distance to water, which
  is why the Field now keeps the previous tick's distances.
- **Passes.** A retexture swaps a palette block for another of the same
  opacity, so it can move no water cell and no leaf; a tree moves no water. Both
  rebuilt all three passes of every touched section, and `upload_chunk` pushed
  all three of all sixteen sections whatever had been rebuilt.
- **The deferred remesh queue.** The world still changes at once; the mesh is
  *owed*, a pass bitmask per section in the Scene, paid off four sections a
  frame. The player's own edits stay immediate -- mining has to be.

### Scheduling, which is the other half

Removing the waste took the tick from 80 ms to ~25, still one frame in ten.
The phases are sequential, independent steps; running them in the same frame
only ever meant one frame carrying all of them. They now sit on separate frames
of the period -- water 0, field 2, retexture halves 4 and 8, vegetation 6 --
and **the drain runs on the odd frames, between them**. That last detail was
worth more than anything else in the final round (15.8 ms -> 11.3): a phase
frame had been paying for its own work *and* for rebuilding what it had just
staged.

Two measurements that saved effort by being taken:

- **Quartering the retexture is worse than halving it**, 131 fps against 137:
  four batches rebuild overlapping sections four times.
- **The relight sweep's prefix copies are not the prize.** `box_sweep_go2`
  already blits only the swept y-slice per level, so rebasing the sweep to the
  box's y band -- planned, and real surgery -- would have bought a fraction of
  what moving the drain did. What did help there was letting `relight_marked`
  blit its swept slice back into the field it started from (~0.5 MB, no
  allocation) instead of allocating a fresh 4 MB field and copying into it. The
  comment saying it could not be the destination was true when written; G67
  (NativeArray reads borrow) has since made it uniquely owned by that point.

### Where the frame goes now

Per ten frames, at 800x600, `CF_VSYNC=0`, seed 7:

| slot | ms | |
|------|-----|---|
| 0 | 7.9 | water tick |
| 2 | 2.4-3.2 | climate field |
| 4, 8 | ~5 | retexture, half the budget each |
| 6 | ~7 | vegetation, every other period |
| 1, 3, 5, 7, 9 | 0.34 + drain | quiet, and the mesh debt |

**77 -> 162 fps, worst frame 55 ms -> 11-13**, measured back to back against
`0131ae8` on the same machine. Absolute numbers on this box swing ~1.5x
depending on what else is compiling; every figure here is an A/B pair taken in
one sitting for that reason.

### The oracles, and why the first two were not enough

`Biome.state_hash` over heights, water flags and biome ids; `World.state_hash`
over every voxel; `mesh_hash` over every vertex of every section. All printed at
`CF_DUMP_FRAME`.

Frame dumps could not do this job: the window lands on a 1x or 2x display run to
run, so two dumps of the same world differ in *size*.

The world hash could not do it alone either. A remesh that skips a pass it owed
leaves every block identical and the screen stale, which is exactly the failure
the pass-selective rebuild could introduce. That is what `mesh_hash` is for, and
it was itself checked: pointing the retexture rebuild at a no-op moved it to
279224865 and left the other two untouched.

The dirty set got the same treatment. A control build with the skip disabled --
every column processed every tick, the old behaviour -- produces identical
hashes at `CF_BIOME_RATE` 100, 2000 and 100000. The last eases a thousand times
faster than the default, so biomes actually flip and the world diverges
completely; the two builds still agree exactly. Ninety ticks at the default rate
would not have tested the easing path at all.

With a bounded queue the invariant changed: not "the mesh always equals a full
rebuild" but "what the queue *finishes* equals a full rebuild". The dump frame
drains to empty before hashing, and drained, it agrees with `CF_REMESH_ALL_AT`.

**Two changes here are not identical, deliberately.** Spreading the tick moves
the world hash -- the phases land on different frames, so retexturing and the
water-flag sampling shift -- and vegetation every other period halves how fast
trees grow. Everything else in the sequence holds all three hashes.

### `scratch/frame_budget.sh`

Runs the real game and asserts no frame after a 60-frame warmup exceeds a
budget (default 16 ms, a frame at 60 Hz), and that the drained mesh equals a
full rebuild. A unit test cannot do this; it needs the window, the GL context,
the actors and the frame loop.

Three things it had to get right, each of which it got wrong first: no
`CF_DUMP_FRAME` in the timed runs, because that frame drains the queue and
hashes four million voxels and is the longest frame in any run that has one (it
reported 56 ms and failed the budget on its own instrumentation); the mesh hash
read off the `state:` line specifically, since plain `grep mesh` also matches
the startup line `mesh all (greedy sections)` and both sides came back empty and
equal; and best-of-N across runs, worst-frame within a run, so a busy machine
does not fail it while a real regression -- which fails every run -- still does.

## Depressions fill, springs can be mined (2026-09-05)

Two reports: flowing water did not fill a hollow, and springs could not be
picked up. Design notes in `2026-09-04-springs-and-flow-design.md` (§1-3, "as
built").

**The hollow.** A probe with a rim spring over a 6x6 bowl filled it in ~900
ticks with ~80% of the trickle evaporating on the way; a 10x10 bowl never
filled, its volume oscillating between 5 and 56 units over 2500 ticks. Two
causes. Spreading only across a difference of two built a pyramid -- seven at
the inlet, one level less per cell outward -- so the pool's rim was thin water
and evaporated; and evaporation was a flat per-cell rate on every thin cell,
so any film wider than `evap` cells lost more than the trickle brought. Fixed
by a levelling pass (one unit to a neighbour one below that has somewhere
lower to send it -- no churn, the queue drains) and by weighting the
evaporation draw by exposure (open sides over four). The 10x10 bowl now
covers its floor by ~750 ticks and holds 890 units at 1500. Terraces two
cells wide remain in a settled pool: the lookahead is one cell.

**The spring.** The break ray ignored water unless hotbar slot 4 was
selected, from before slots held items. Springs now stop the ray always; other
water only while a water item is held.

Cost, release, seed 7, `CF_WORKERS=1`, frames 800-900: 8 chunks ticking, ~120
active cells, actors ~2.0 ms, 16-23 sections owed -- against 87 cells and 1.4
ms before. Brooks are about twice as long: a brook cell has two open sides and
now loses at half the old rate. Within the 2 ms target; `CF_EVAP` is the knob
if it is not.

## Block light (fungus phase 1, 2026-09-05)

A second per-voxel light channel, seeded from `Light.emission(id)` instead of
the sky and propagated by the same level-synchronous sweep (GAPS G63). Design
`docs/superpowers/specs/2026-09-04-fungus-design.md` §6, plan
`docs/superpowers/plans/2026-09-05-fungus-phase1-block-light.md`.

| | before | after |
|---|---|---|
| startup light flood (debug, 8x8 chunks) | 521 ms (skylight only, §Lighting) | **642-708 ms** (skylight + block light, three runs) |
| placing a glow cap: edit + relight both fields + section remesh (debug) | — | **52-61 ms** |
| `scratch/frame_budget.sh`, 12 ms budget, release | 8.84-10.8 ms | **8.84 ms** best of 3, drained mesh equals full rebuild |

The block flood costs roughly what the skylight flood does, because the sweep
is the same fourteen passes over the same prefix of the field; a world with no
emitters still pays it. The scan bound is `sky_floor + max_level()` rather than
`sky_floor`, since block light climbs into the open air above the terrain where
skylight has nothing to do -- the first flood test caught exactly that: a cap
in the open lit five blocks up as 0.

Verification: eleven new tests. Every incremental block relight (place, remove,
wall off, world edge, a second emitter just outside the box) equals a full
re-flood, `World.relight_marked` repairs both fields and unions the section
marks, and a lit corner refuses to greedy-merge with dark ones. End to end:
`CF_AUTOGLOW=100` places a glow cap three columns east of the player through
`edit_block`; against the same pinned night scene without it (`CF_SEED=7
CF_SUN=180 CF_TIME=0 CF_WEATHER=0 CF_AUTOSPIN=8 CF_NOMOUSE=1`, dump at frame
200) the frames differ in **396 895 pixels**, all of them the lit ground and
slope beside the cap. `docs/lighting-glow.png` is that frame.

Two things worth knowing for the next emissive block:

- The vertex layout did not grow. The shade float carries both channels as
  `2 * round(blk * 255) + sky`, so every overlay that already pushed a plain
  shade in [0, 1] decodes as sky-only with no change. The shader unpacks with
  `floor(v * 0.5)`; interpolation across a triangle is exact because the
  encoding is linear in both parts and the sky part never leaves [0, 1].
- Greedy keys went from 6-bit to 10-bit corners (48 bits with the id). Block
  light varies per vertex like skylight does, so it limits merge runs the
  same way; the budget above says that is affordable at one emitter, and the
  fungus phases that add thousands will have to measure it again.

An `env $E` with an unquoted variable in zsh does NOT word-split: all the
knobs became one `CF_NOMOUSE` value, the run had no seed, no sun pin and no
frame cap, and the "off" dump was a daytime frame of a different world. Write
the knobs out, or quote-split with `${=E}`.

## Mycelium field (fungus phase 2, 2026-09-05)

A per-column field beside the biome field: species, vigour (float 0..1),
reach, hold, claimant. Design `docs/superpowers/specs/2026-09-04-fungus-design.md`
§2-3, plan `docs/superpowers/plans/2026-09-05-fungus-phase2-field.md`.

| | cost |
|---|---|
| `myc tick`, empty field (debug) | 2.3 ms with a 3x3 test per column; **0.21 ms** after a per-row pre-pass |
| `myc tick`, one growing patch of ~120 columns (debug) | **0.8-0.95 ms** |
| `biome tick` beside it, for scale (debug) | 6.3 ms |
| `scratch/frame_budget.sh`, 12 ms budget, release | **8.85 ms** best of 3, drained mesh equals full rebuild |

The tick is **pull-based**: every column reads its eight neighbours from the
previous tick's arrays and writes only its own entry into five fresh arrays.
That is the light-field idiom (GAPS G21/G63) applied to a 2D field: no array is
read and then written in one pass, and the Scene's shared reference never
forces a copy (G68). The five fresh arrays are ~200 KB a tick, the same order
the biome tick already allocates.

The per-row pre-pass is the whole of the empty-field cost story. One byte per
row from one pass over the species array, and a row with nothing in it or
beside it skips all n of its columns at once. Ten times cheaper than testing
each column's 3x3 neighbourhood, and most of the world is empty most of the time.

Reach is in **half-hops**: a straight hop costs 2, a diagonal 3, so a planting
of radius 8 grows as an octagon. The first cut charged every hop 1 and grew
squares (Chebyshev), the second charged diagonals 2 and grew diamonds
(Manhattan); both looked drawn rather than grown. `docs/fungus-myc-map.png` is
the octagon, seed 7, `CF_AUTOPLANT=100 CF_MYC_RATE=5000`, dumped at frame 3000
with `CF_AUTOMAP CF_BIOME_MAP=1 CF_MYC_MAP=1`: a Meadowbell patch of 118 columns
on grassland beside the marker, darker along the forest edge where its fitness
falls off.

Two test findings worth keeping:

- **Test-module aliases leak across the combined test binary.** `alias
  CubeForge.Myc as M` in `myc_test.march` resolved to `Mat4` at link time
  (`_CubeForge.Math.Mat4.vigour_u8` undefined), because `mapview_test.march`
  already aliases `Mat4` as `M` and every test module compiles into one unit.
  Test aliases must be unique across `test/`. Recorded as GAPS G69.
- Two contest tests were wrong before the tick was: one placed the challenger
  ten columns from the incumbent with a reach of eight, the other expected a
  species with fitness 0.33 to spread past the 0.6 spread threshold. Both
  "failures" were the rules doing what the spec says; the tests were corrected
  to assert what reach and the threshold actually predict.

## Mycelium blocks (fungus phase 3, 2026-09-05)

The field reaches the world: a held column's surface becomes "mycelium over
<base>" and a lost column's surface goes back, through a budgeted queue on the
retexture slots. Plan `docs/superpowers/plans/2026-09-05-fungus-phase3-blocks.md`.

| | cost (debug) |
|---|---|
| `myc migrate`, no glow change: set_block + shown + geometry marks | **0.013 ms per column** (12 columns in 0.16 ms) |
| `myc migrate`, a glow change: the above plus a block-light relight | **10.4 ms per column** |
| `scratch/frame_budget.sh`, 12 ms budget, release, no fungus in the scenario | **8.81 ms** best of 3, drained mesh equals full rebuild |

So two budgets. `CF_MYC_BUDGET` (32 per period, split over the two slots) bounds
the cheap kind, and `CF_MYC_GLOW_BUDGET` (default **1** per slot, measured at 2
and lowered) bounds the relights; a glow column skipped for budget stays a
candidate and lands on a later slot. A 120-column glowing patch therefore takes
about ten seconds to light up fully, which reads as the glow "coming on".

**Species is not in the block id.** Ids 23..43 encode base x glow (seven bases,
three glow levels), so `Light.emission` stays a pure function of the id and the
base can be restored when the network leaves. The species the surface shows
lives in `World.shown`, one byte per column, written in the same migration step
as the block; the mesher reads it into the greedy key (bits 48..55) and picks
one of 42 per-(base, species) texture layers. A species change with no glow
change is therefore no block edit at all -- `shown` moves and the section is
marked, the migration list keys on `wanted != shown` -- which is what keeps a
contest border cheap. The rejected alternative, species in the id, was 42 ids
now and seven more per future species.

Texture generators never read the texture array: each mycelium layer
reproduces its base from the base's formula and overlays threads, because a
read before a write copies all 60 KB per texel (GAPS G21).

Mycelium emission is **dim 3, bright 6**, not the spec's 4: at 4 a lone bright
column lifted the ground by about 25/255 at night, which did not read as lit.
`docs/fungus-myc-ground.png` is a Meadowbell patch by day (seed 7,
`CF_AUTOPLANT=100 CF_MYC_RATE=5000`, frame 2000); `docs/fungus-myc-glow.png`
is a forced Lanterncap column at night (`CF_AUTOPLANT_SPECIES=4`, frame 400,
before it withers: it is unfit on grassland and is shown from roughly frame
120 to 640).

Finding: a dump at frame 1200 of that run showed no glow and a world hash equal
to the run without the planting. The column had been shown and then migrated
back, so the blocks matched again -- the hash cannot see a change that undoes
itself. The `mycelium at <column>` line in the dump summary (species, vigour,
wanted, shown, surface id, block light) is what settled it; it stays.

## Spores and planting (fungus phase 4, 2026-09-05)

The loop closes. Plan `docs/superpowers/plans/2026-09-05-fungus-phase4-spores.md`.

| | cost |
|---|---|
| startup, seed 7, debug: `Biome.build` | 675 ms (unchanged; it now runs before the light flood instead of after meshing) |
| startup: `Myc.wild` — 14 patches, 2,527 columns across six species | **8 ms** |
| startup: applying those as blocks and `shown`, one batched write per chunk | **5 ms** (700 ms through `World.set_block`, one 64 KB chunk copy per column) |
| `scratch/frame_budget.sh`, 12 ms budget, release, wild fungus live at seed 7 | **9.24 ms** best of 3 (9.37 and 12.09 seen), drained mesh equals full rebuild |

Wild patches are written before the light flood on purpose: glowing wild
mycelium is lit by the flood for free, where relighting it column by column at
the glow budget would take a ten-second sunrise on every new world.

The budget's worst run moved from 8.8 to 12.1 ms with fungus live. The pinned
scenario now migrates real patches on its retexture slots, one glow relight per
slot among them, and a phase frame that already carried the biome retexture
can now carry that too. Best-of-3 still clears, which is what the script
asserts, but the headroom the budget was built on is spent; the fruit phase
measures before it adds anything to a phase frame.

`docs/fungus-wild-map.png`: the wild patches on the map at seed 7 -- Meadowbell
on the grassland, Frostcap on the snow, Marshlight on the wetland, Lanterncap
in the forest, Sunshelf on the sand. `docs/fungus-spores.png`: the item path end
to end (`CF_WILD=0 CF_AUTOSPORE=100 CF_MYC_RATE=5000`, frame 2000): three
Meadowbell spores given, one used on the column three east through the same
function a hotbar spore takes, the hotbar showing the spore icon with a count
of 2, the patch grown, and the ground readout `MEADOWBELL 100` at the top left.

The readout is uppercase because the HUD glyph set is; punctuation renders
blank, so `Lanterncap 25%` reads `LANTERNCAP 25`. Its buffer rebuilds only when
the string changes, in `refresh_hud` beside the hotbar, which already had the
right shape for "rebuild on change and upload".

The "nothing grows here" readout is unreachable with six species: every
climate has at least one band over it. It stays for a roster that leaves gaps.

## Fruit (fungus phases 5-6, 2026-09-05)

Mushroom bodies of three tiers on mature mycelium. Plan
`docs/superpowers/plans/2026-09-05-fungus-phase5-fruit.md`.

| | debug | release |
|---|---|---|
| one body grown or felled (stamp/fell + both-field relight + occupancy sync + marks), any tier | 32-44 ms | **6-7 ms** (a tree: 7-8 ms) |
| the candidate scan, 2,048 columns per fruit slot | 2-3 ms | **0.3-0.6 ms** |
| `myc tick` on the field slot with fourteen wild patches live | -- | 3.2 ms (beside the 1.7 ms biome tick) |
| `scratch/frame_budget.sh`, 12 ms budget, release, wild fungus and fruit live | -- | **11.09 ms** best of 3 (12.21 and a 55 ms outlier seen; the pinned run's own worst frame was 10.0 ms when re-run with timings) |

The fruit phase runs on the vegetation slot on the periods vegetation skips, so
no frame carries a tree and a mushroom at once; a body is a tree-sized edit and
the budget is one per slot.

**A cursor bug, found here, pre-dates the fungus.** Both every-other-period
phases keyed their rolling scan window on the tick count: `(tick * 2048) %
16384` over eight windows, visited on even ticks only, reaches the even four.
Vegetation had been growing and felling in half the world since it landed
(RESULTS 2026-09-04 measured it without noticing); fruit inherited the bug on
the odd ticks and it showed at once, as a mature patch whose centre never
fruited. Both now key on the period count. Trees grow everywhere from this
commit, which moves the pinned scenario's mesh hash.

**Glow migrations batch under one relight.** A planted glowing patch changes
the emission of every column it covers, and phase 3's rule -- one relight per
retexture slot -- would have taken fifteen seconds to show a 181-column
Marshlight patch. Now the first glow column in a slot anchors, further glow
columns within 4 of it are taken too, and the slot ends with a single
`relight_block_marked` at the anchor: every emitter in that 9x9 is within 4
of the anchor and shines at most 6, so the radius-15 relight box covers them
all. `CF_MYC_GLOW_BUDGET` is gone; `CF_MYC_BUDGET` defaults to 64 per period.
Migration of 16 glowing columns: 12 ms debug in one slot.

`docs/fungus-fruit.png`: small Meadowbell bodies on a planted patch by day
(`CF_WILD=0 CF_AUTOPLANT=100 CF_MYC_RATE=5000 CF_FRUIT_RATE=2000`, frame 2000).
They are cutout cubes, the choice the vegetation design made for leaves; the
silhouette reads as a mushroom from the side and as a splayed shape from above,
which a top-face cap texture would fix. `docs/fungus-giant.png`: a mature
Marshlight patch at night (`CF_AUTOPLANT_SPECIES=5 CF_AUTOPLANT_MATURE=1
CF_MYC_RATE=0 CF_FRUIT_RATE=5000`, frame 1200): the mycelium carpet-glows at
emission 6, a giant stands on the crosshair column (`surface 61 at y 90, block
light above 9` in the dump), its cap above the horizontal view. On the wild
world at seed 7 a 2,100-frame run at `CF_FRUIT_RATE=2000` grew 45 bodies: 39
small, 5 medium, 1 giant.

`CF_AUTOPLANT_MATURE=1` plants a full-vigour patch at once, which with
`CF_MYC_RATE=0` stays whatever the climate says: the way to put a chosen
species' fruit in front of the camera without waiting.


## Mycelium tick: the active set (2026-09-05)

Plan `docs/superpowers/plans/2026-09-05-myc-tick-active-set.md`. Release,
pinned wild world at seed 7 (fourteen patches, 2,527 columns), the `myc tick`
print on the field slot:

| | before | after |
|---|---|---|
| first ninety seconds, wild-patch rims easing from full vigour to their fractional fitness | 3.2 ms | **1.1-1.3 ms** |
| settled (dirty 3-5 columns) | 3.2 ms | **0.30-0.35 ms** |
| during a fast planted growth (`CF_MYC_RATE=5000`) | -- | 0.17-0.37 ms median, 4.6 ms on the tick the patch is planted |
| `scratch/frame_budget.sh`, 12 ms budget | 8.74 ms | **8.85-9.09 ms** |

Four things had to be true before a settled world ticked cheaply, and each
one was found by the counts in the print (`biome active`, `myc dirty`):

- **A dirty flag per column**, set when the column changed or is unsettled,
  and a column is looked at when it or a neighbour is dirty. The fresh arrays
  start as memcpys of the old ones (`u8_blit` / `f32_blit`; vigour moved to
  f32 to be blittable), so a column not looked at costs nothing. This alone
  went 3.2 -> 2.1 ms: not enough, because
- **the biome flags ~3,500 columns active** for the first half hour of a
  session (its easing toward moisture targets that the water actors keep
  moving), and waking on those flags re-evaluated every patch column under
  them every tick. The tick now remembers the climate each column was last
  evaluated at and wakes only when it has moved by `climate_eps()` = 0.004
  -- about 0.07 of fitness on a band edge, every fifty ticks under the
  biome's day-long easing instead of every tick. No reporting contract with
  the biome: a climate that moves under a column wakes it by itself.
- **f32 storage against f64 targets**: a column whose fitness is not
  f32-representable could never sit exactly on it and never settled. Settled
  is now within `settle_eps()` = 0.002 of the target, below what the map or
  the readout can show.
- **Neighbours read only species, reach, and whether vigour clears the spread
  threshold**, so a column easing toward its target re-evaluates itself
  (dirty 1) without waking its 3x3 (dirty 2).

The remaining 0.3 ms is the per-column scan for dirty flags and climate
movement (16k columns, six reads each) plus the memcpys; the 1.2 ms of the
first ninety seconds is ~600 rim columns legitimately easing at 1/540 per
tick. `tick_all` looks at every column and is the oracle: under a static
climate the two agree bit for bit over 300 ticks of a contest (tested); under
a drifting climate the active tick lags by at most `climate_eps` of climate,
by design.

## Save and load: a start screen and five slots (2026-09-05)

Design in `docs/superpowers/specs/2026-09-05-save-load-design.md`, plan in
`docs/superpowers/plans/2026-09-05-save-load.md`.

**What shipped.** A start screen (CUBE FORGE, NEW GAME, LOAD GAME, QUIT, the
seed field) over the freshly generated world; SAVE GAME in the escape menu;
one slots page for both, five rows and BACK, each used row naming its seed and
save time. A slot is a directory of 64 raw chunk files plus a text header
written last, 4 MiB. A save restores the blocks, the player, the inventory,
the seconds into the day and the weather phase. `CubeForge.Save` owns the
format; `Menu` grew a mode and a page; `run_session` takes a seed or a slot.

**Round trip** (`scratch/saveload.sh`, seed 7, save at frame 201, load at
frame 30 of a fresh process):

| | |
|---|---|
| save, 64 chunk files + header | 8.7-12.7 ms |
| read + validate a slot | 4-5 ms |
| session rebuild after the read (light, occupancy, mesh, upload) | as a new game, ~0.6 s |
| world hash at save vs at load | 196060934 = 196060934 |
| player position at exit, both runs | (48.5, 82, 78.5) |

The frame dumps differ by ~15k pixels because the loaded session's first
water tick and its spray differ, not the terrain; the world hash is the
oracle.

**Water actors read their own chunk file.** Messages cannot carry byte
arrays (GAPS G44), so `WLoadSlot` has each actor read its chunk and its four
neighbours' edge columns from the slot: 64 x 5 reads of 64 KiB. Marked in the
code as a stopgap until a message can carry the chunk the frame loop already
read.

**Two findings on the way.**

- *The escape menu had been invisible.* The menu uploaded to VBO slot 247,
  and the spray pool added later took `CF_SPRAY_SLOT 247` in the shim, so
  spray's per-frame upload replaced the menu's vertices. The CF_AUTOMENU
  restart test checks hashes, not pixels, so nothing caught it. Found here
  from a frame dump of the start screen; fixed on main in the same hour by
  05c9637, which moved spray to 246, so the menu stays on 247.
- *Frame dumps of a menu are possible headlessly*: `CF_AUTOSLOTS=<frame>`
  opens the slots page at that frame, the way CF_AUTOMENU opens the menu, so
  a layout change can be looked at from a script.

**Cost.** Nothing on the frame path: the slot listing is read once when the
page opens, and the save is synchronous on the click (one ~10 ms frame).

## Fungus follow-ups: cap faces, bodies fall with the network, the scan pre-pass (2026-09-05)

| | before | after |
|---|---|---|
| fruit candidate scan, 2,048 columns per fruit slot, release, wild world | 0.3-0.6 ms | **0.03-0.05 ms** |
| `scratch/frame_budget.sh` | 8.85-9.3 ms | 9.35 ms best of 3 (two runs at 11.3, the phase costs unchanged) |

**Small mushrooms** show their cap layer on the top and bottom faces and the
silhouette on the sides; from the air they read as a cap now, not a splayed
shape. No new layers: the cap block's texture already existed.

**Bodies fall with the network.** The fruit scan used to read two blocks per
column to find bodies whose ground had lost their species and list them for
felling. Now the migration that clears a column's ground fells the body
standing on it in the same step (its cap glow joins the slot's relight
anchor), and the scan only grows. That let the scan take a per-row species
pre-pass like the tick's: an empty row costs one byte, and a column is read
from the world only after its species, vigour, shown, canonical and roll
checks all pass. Verified with a mature Lanterncap patch on grassland at
`CF_MYC_RATE=300 CF_FRUIT_RATE=100000`: three bodies at frame 900 with zero
orphans; by frame 5000 the patch had withered to the 24 lakeside columns
still damp enough, one body stood on them, and zero orphans -- the two whose
ground went were felled with it. `orphans` is now in the dump summary.

**A bug found on the way.** The migration read the surface at the heightmap's
top, and a body standing on a column IS the heightmap's top there. The cap
could not carry mycelium, so the column's `shown` was cleared, and the next
scan saw a body on ground that showed nothing and felled it. Every body
decayed within a scan window of growing. The migration, `can_plant` and the
dump diagnostic now read the ground through `Fruit.base_of`.


## Fungus climate feedback (2026-09-05)

The network changes the ground it holds. Plan
`docs/superpowers/plans/2026-09-05-fungus-climate-feedback.md`.

| species | temperature pull | moisture pull |
|---|---|---|
| Frostcap | -0.09 | 0 |
| Pinewart | -0.04 | +0.07 |
| Meadowbell | 0 | -0.09 |
| Lanterncap | 0 | +0.09 |
| Marshlight | +0.03 | +0.12 |
| Sunshelf | +0.09 | -0.09 |

Distinct species in a column's 3x3 sum, capped at 0.3 per axis; `CF_MYC_FEEDBACK`
scales. Every pull points at the species' own core, which is the stability
argument the original spec asked for: a species never weakens its own footing,
so the loop only reinforces and cannot cycle; where two species pull against
each other the sum favours one, which then strengthens itself through the
contest that already resolves mixed ground.

Seed 7, spawn, ground moisture 0.25 (grassland), `CF_BIOME_RATE=100000` so the
climate eases within the run:

| planting | offset (moisture) | eased moisture | biome |
|---|---|---|---|
| a mature Marshlight patch alone | +0.08 (at the first tuning) | 0.33 | grassland |
| Marshlight + Lanterncap + Pinewart, one interleaved patch (`CF_AUTOPLANT_SPECIES2/3` with `CF_AUTOPLANT_MATURE`) | +0.28 | **0.53** | **forest** |

The first tuning (pulls 0.05-0.08, cap 0.2) reached +0.19 with the three and
left the ground at 0.44: nothing could cross from a band's middle. The pulls
went up so that the largest single pull (0.12) is under the 0.25 from
grassland's centre to the damp threshold and the three damp species together
(0.28) are over it. Three patches planted three columns apart did NOT mix:
`plant_patch` overwrites, so the collection only met along thin rings. The
interleaved patch is what a player would plant on purpose, alternating
species; it is `Myc.plant_mix`. `docs/fungus-feedback-map.png` is the map at
frame 1200: the patch, and the forest colour under it where grassland was.

**Cost.** The first version scaled and copied the whole 32k-entry offset array
every tick and had the biome compare four floats per column: the field slot
went from 1.7 to 8.7 ms in release and the budget failed at 12.9. Neither pass
was needed. The field keeps its raw pulls up to date where species change
(nine columns per change), reports which rows moved, and the biome scales and
clamps only the columns it evaluates, looking at a whole row when its pulls
moved. Field slot now: biome tick 2.7 ms (1.7 before feedback; the extra is
the patch rows it evaluates while their offsets settle), mycelium tick 1.2-1.4
ms; budget **9.84 ms**.

Two lessons for the next per-column array: a plain 32k `set_f32` pass in March
costs 1.8 ms in release, so anything per tick must be proportional to change,
not to the world; and a merge of three scripted plantings is not a mix.

**Save/load, merged in from main during this work.** A load rebuilds the
mycelium field from the seed (`Myc.wild`), so wild patches return but any
planted or spread network is lost, and its surface blocks migrate back to
bare ground; the biome's eased axes are rebuilt at target the same way. Both
are the "cannot be rebuilt from the voxels" risk the two specs carry. Listed
in `todos.md`.


## Save the fields, and food effects (2026-09-05)

Plan `docs/superpowers/plans/2026-09-05-fungus-save-fields-and-food.md`.

**The fields ride in the save.** One more file per slot, `fields.bin`, 131,072
bytes for the 128-column world: species, vigour (0..255), reach and shown
species at one byte per column, then the biome's eased temperature and
moisture at two bytes each. Written after the chunks and before the header,
so the header still marks a complete save; a slot without it (an older save)
loads with a printed note and rebuilds both fields from the seed as before.
Round trip on seed 7 with a planted patch grown to frame 1500: the `myc`
state hash and every species count are identical across the load, and the
climate carries on from its saved position (moisture 0.2433 at the save,
0.2422 a hundred frames into the loaded session, easing as it was). Load
cost: 11 ms for the slot including the fields. Hold, claimant, the climate
memory and the pulls are rebuilt on load and the first tick looks at every
column once.

`import` is a keyword in March: `fn import(...)` is a parse error with no
hint that the name is the problem (GAPS G65 has the same shape). The pair is
`to_bytes` / `of_bytes`.

**Food.** A cap is food. Using one with nothing in reach eats it: one is
consumed and the species' effect starts, or refreshes, for thirty seconds.
(That gesture was replaced on 2026-09-06 by an eat key and a right-click in
the inventory window -- see "Eating from the inventory" below.)
`CubeForge.Effects` holds four until-times and answers multipliers for a
clock; `Player.update_with` takes them (walk speed, jump speed, swim speed,
both horizontal and vertical); the lantern effect forces the flashlight on;
the ground readout line shows the active effects with seconds left. Species
to effect: Meadowbell and Sunshelf speed x1.5, Frostcap and Pinewart jump
x1.35, Marshlight swim x1.6, Lanterncap lantern. Effects are not saved.
`CF_AUTOEAT=100`: `ate a Frostcap cap: JUMP 30`, and the dump 200 frames
later shows `JUMP 28`.

Budget: 10.25 ms best of 3 (a 32 ms outlier in one run; the pinned run's own
worst frame 9.67 ms). The allocation gauge reads 127 live objects per frame;
the effects summary is rebuilt as a string every frame for the readout
comparison, which is the obvious thing to make per-second if that number
ever matters.


## Effects summary, stamp-gated (2026-09-05)

`Effects.stamp` packs the whole seconds left on each kind into one integer:
four float reads, no allocation, and it moves exactly when the summary text
would (a sweep test over forty seconds with two effects checks every step).
The frame loop rebuilds the summary only when the stamp at `now` differs
from the stamp at the previous frame's clock -- which also catches a bite
this frame, whose timer was a second longer a frame ago -- and the UI keeps
the last string otherwise. The allocation gauge did not move (127 live
objects per frame with an effect active, the same as before): the ground
readout string is the per-frame allocator, not this one. Listed.


## The reticle over fungus (2026-09-05)

The crosshair leans 45% of the way from white toward the cap colour of the
fungus under it: a fruit block's own species, a mycelium block's column
species from `World.shown`. `Hud.reticle_r/g/b(tint)`; the species is folded
into the HUD rebuild key, so the bars recolour only when the target's species
changes and nothing is rebuilt per frame. `docs/fungus-reticle.png`: looking
straight down at a Lanterncap patch (`CF_PITCH=-140`, a new knob for the
spawn pitch in hundredths of a radian), the reticle is a warm cream against
the white it keeps over sky and water. Budget 11.26 ms best of 3 on a machine
still carrying other sessions' benchmarks.

## The world tick, part two: the fungus era (2026-09-05)

The tick work recorded above was measured against a world without the fungus
system. When that landed, main measured ~11% slower than the day before --
252-259 fps to 224-228 at 1920x1200, interleaved against a rebuild of the
previous tree on the same machine at the same moment. The gap was identical at
320x240 and with shadows off, so it was CPU, not fragments.

It was not a regression. It was new features doing real work, and they were
already routed through the machinery this branch had built: the mycelium
migrations, fruit growth and glow relights all stage into the remesh queue
without ever having been told it exists. What had grown was the climate tick.

### The dirty set's assumption expired

`Biome.tick` eases only awake columns, and it was built on the observation that
almost everything settles. The fungus climate feedback moves a row's target by
more than one ease step, so a column chasing it is never on it: **about four
thousand of the sixteen thousand columns are permanently awake** where before
almost none were. The dirty set still earns its keep -- four thousand beats
sixteen -- but the sweep it guards had grown back into the biggest phase in the
tick, 1.9 ms before the fungus work and 2.8 ms after, all on one frame.

Split in two. `tick_begin` keeps the whole-map half -- rescan the water flags,
recompute the distance field, wake what moved -- on one frame, because that is
O(map) whatever is awake. `tick_slice` sweeps the awake columns of one slice,
and a slice runs every frame, covering the map once per period. Each column is
still visited exactly once per period, so the ease rate and the hold counter are
unchanged.

`Myc.tick` had to move with it: the two fields alternate -- the biome reads the
network's pulls, the network reads the climate the biome eased -- so the sweep
must be COMPLETE before it looks. It runs on the frame the last slice lands.

| | fps | worst frame |
|---|---|---|
| before | 230-235 | 10.2-19.2 ms |
| after  | 247-250 | 9.7-10.7 ms |

The field slot fell from 4.6-5.2 ms to 1.57 ms. **The mechanics are proved
rather than argued: with nothing easing (`CF_BIOME_RATE=0 CF_MYC_RATE=0`) the
sharded and unsharded builds produce identical hashes on all four fields**,
which is what says the slices cover every column exactly once, none missed and
none twice.

### Mining, the last synchronous thing

Everything else had been bounded by the queue and a block edit had not, so it
became the tallest thing in the game: 11.5 ms mean, 16 ms worst. Measured, it
split 1.4-2.3 ms of block and occupancy writes, 5.7-6.9 ms of relight, 1.4-5.9
ms of remesh -- and the relight had doubled when the fungus work gave the world
a second light field.

- **The block-light pass is a provable no-op in the dark.** With no block light
  anywhere in the scan box there is nothing to clear, nothing to seed (an
  emissive block inside the box would have lit its own cell), and nothing
  outside can reach in without lighting the ring, which the scan box contains.
  The edit's own block is the one thing that can add light without being in the
  field yet, so it is tested separately. Where it fires: **1.76-2.57 ms ->
  0.15-0.23**, the cost of the box scan that proves it.
- **The remesh splits.** The edited section is rebuilt in the frame the click
  landed, so the block still vanishes under the cursor with no delay; its
  neighbours go through the queue. Verified where it matters: with
  `CF_AUTOBREAK` mining throughout, the drained mesh equals a full rebuild
  (935674909), so what an edit defers converges on what doing it synchronously
  produced.
- **Starting the bounded sweep at the box's brightest voxel** was worth 0.6 ms
  and did NOT survive: the worklist sweep landed on main the same day and
  supersedes it outright, since a worklist never visits an empty level at all.
  The measurement is kept for the reason it was small, which still applies to
  anything bounding that box: it reaches fifteen blocks ABOVE the edit, so it
  holds lit sky until the player is more than fifteen deep, and a descent spends
  most of its time with a full-brightness box.

  block edit   mean 11.5 -> 9.4 ms, worst 16.1 -> 10.8 ms

### Four things measured and NOT done

Each of these looks alarming in the source and is not worth touching. They are
recorded so the next reader does not spend the day finding out again.

- **Budgeting the water tick.** Planned off a 7.9 ms figure that predated the
  remesh queue. Re-measured after it: **calls 0.7-1.6 ms, apply 0.07 ms**. The
  7.9 was the remesh, and the queue had already taken it.
- **Per-chunk biome field storage.** A whole-array copy per edit looks like the
  obvious cost -- `note_edits` and `rescan_box` each copy 16,384 entries. They
  cost **0.28-0.33 ms and 0.14-0.15 ms**, about 0.067 ms a frame. Per-chunk
  storage would remove nearly all of it and buy nothing at this world size. It
  is a tidiness change, not a performance one.
- **A settle tolerance on the climate offsets.** Meant to cut the four thousand
  awake columns. At 0.002 it does nothing (3213 -> 3288). There is a knee, and
  it is in the wrong place: 0.02 gives 2338, 0.1 gives 165 -- and 0.1 is a THIRD
  of the gap between classification thresholds, so a column could settle that
  far from its target and land in the wrong biome. The deeper reason not to
  bother: once the sweep is sharded the awake set costs **0.13-0.16 ms a
  frame**, so cutting it by a third saves 0.04. The 2.8 ms that motivated the
  work was already stale when the work was proposed.
- **Parameterizing the world size.** See the world_size_test commit: refinement
  predicates cannot reference a constant function, and deriving the bounds took
  the build from 85 unverified obligations to 100.

### On measuring at all

Two things cost more time than any optimization here.

**This machine's background load swings results by 1.5-2x.** The same binary
measured 95 fps and 198 fps on the same day. Every number in this section is an
A/B pair taken back to back, interleaved, because nothing else is trustworthy.
A report of "perf issues on main" was chased to a load average of 72 from three
other sessions compiling; current main and the pre-merge build measured
identically.

**A budget test that straddles its threshold is worse than none.** At 12 ms
`scratch/frame_budget.sh` gave 11.52, 11.88, 11.94, 12.07, 12.17, 12.23 and
12.82 across one day, on code whose only measured change was elsewhere;
best-of-6 did not settle it. It now asserts 16 ms -- a frame at 60 Hz, the
property that survives a busy machine -- and the sharper number lives here,
where it cannot rot into a false alarm. For the 120 Hz frame run
`frame_budget.sh 8.3` on an idle box and read it as a measurement, not a gate.

## Oasis and fungal grove (2026-09-05)

`docs/superpowers/specs/2026-09-05-oasis-and-fungal-grove-design.md`, plan
`docs/superpowers/plans/2026-09-05-oasis-and-fungal-grove.md`. Two biomes the
field earns: an oasis around a small body of water in a hot region, a grove
where the mycelium is established.

- **Water bodies.** The water flags are labelled into 8-connected bodies each
  tick by rounds of min-label propagation (a forward and a backward pass) and
  pointer jumping over one threaded `NativeIntArr` — a queue is G63 — and a
  body of at most 48 columns is small. The distance sweep carries a small bit
  in the distance byte, and a small body's moisture reaches 4 columns instead
  of 24. Measured with a temporary print: the labelling is **0-1 ms**, the
  sweep 1 ms, at seed 7.
- **Distances are reused** when this tick's water flags equal the last tick's
  and no edit has moved a flag since (`Biome.stale()`, 255, written into the
  distance byte by `note_edit` and `note_edits`). The first cut compared flags
  only and the canal test caught it: an edit sets the flag without a sweep, so
  "flags unchanged" did not mean "distances current".
- **The grove wake.** The biome tick reads the network's species and vigour
  and is woken by `Myc.dirty_rows`. Waking every column of a dirty row cost
  **8 ms a tick against 2.6-3.2** at base (`CF_AUTOFLOW`, seed 7): the wild
  patches ease vigour for hundreds of ticks, so most rows were dirty. Now a
  dirty row's columns are looked at only where the grove test disagrees with
  the stored biome (`Biome.must_look`): **2.1-4.1 ms**, the same as base.
- **Grove, end to end.** Seed 7, `CF_WILD=0 CF_AUTOPLANT=100
  CF_AUTOPLANT_SPECIES=4 CF_AUTOPLANT_MATURE=1 CF_BIOME_RATE=100000
  CF_MYC_RATE=0`, frame 1200: the player's column reads `grove`, two medium
  Lanterncap bodies stand.
- **Oasis, end to end.** Seed 199 spawns in desert (`temp 0.67 moist 0.17`).
  `CF_AUTOCANAL=10 CF_BIOME_RATE=100000 CF_VEG_BUDGET=8`, frame 1400: the
  player's column reads `oasis`, the ground round the canal has migrated to
  grass with bushes, and the biome map shows the bright green pocket inside the
  tan desert (`docs/oasis-grove-map.png`, which also shows a wild grove in
  violet; `docs/oasis-ground.png`). One tree grew during the run; whether it was
  the oasis palm was not confirmed from the frame — palm growth goes through the
  same `Veg` path as oaks and is covered by `veg_test`.
- **Seed 7's biome map is pixel-identical** before and after the small-body
  change in the map view's frame around the spawn (`scratch/cmpframe.py`): the
  coast and the lake's reach did not move. The spring brook at (116, 74) is
  outside that frame.
- 349 tests (329 before).


## Perf pass over the fungus features, on March origin/main 9ca8a98d (2026-09-05)

**Toolchain.** `.march-version` moves from `watch-ac782d80` to `watch-9ca8a98d`,
March origin/main at the time: 21 commits, among them aggregate reference
counting and deep drop, owned aggregate parameters, record FBIP reuse, and a
scheduler count that follows the CPUs. Built with `make install
PREFIX=~/.march/versions/watch-9ca8a98d` from a detached checkout; dune puts
the stdlib under `share/march`, and the toolchain layout forge expects wants
it at `<version>/stdlib`, so that is a symlink (without it every module fails
with "Unknown module `Array`", which reads like a project bug). The project
builds, lints and passes 365 tests on it unchanged. Paired runs on a machine
carrying other sessions' benchmarks (load average 15-20; every number below
is from such pairs, back to back): startup, fps, the allocation gauge (132
live objects per frame) and the budget all within noise of the old pin. The
new runtime changed nothing this project can measure.

**Where a body's cost was.** A fruit body growing cost 6-8 ms and a tree 7-8;
instrumented, a body was stamp 0.7 ms, relight 6-10 ms, occupancy sync 0.5,
biome rescan 0.3. The relight was skylight 4.6 ms + block light 2.5 ms on
average, and the block pass's "nothing glows here" exit almost never fired
near a wild patch, because glowing mycelium is everywhere. Both channels ran
the same sweep: fourteen level passes, each visiting every voxel of the
33-wide scan box and each preceded by a slice copy.

**The worklist sweep.** Per-level lists of the voxels that hold each level,
gathered in one pass after seeding and appended to as writes happen, so a
level costs its own list and not the box; and in place on one buffer, no
slice copies. `Light.sweep_box_lists`, shared by both relights; the old
two-buffer sweep is gone. The "incremental equals a full flood" tests for
both channels are the oracle and pass unchanged.

| | before | after |
|---|---|---|
| skylight relight, median of 112 | 4.6 ms | **3.5 ms** |
| block-light relight, median | 2.5 ms | **1.5 ms** |
| a body, both relights | ~7 ms | **5.2 ms** |
| the sweep alone, skylight | ~4 ms | 1.1-2.1 ms |
| the sweep alone, block light | ~2 ms | 0.15-0.2 ms |

What is left of a relight is the clear, the seed, the before-copy and the
section diff, each a pass over the box; the sweep is no longer the biggest.

**Two traps on the way, both worth a GAPS entry (G70).** The first version
returned a two-field pair per level and cost 1.2 s a relight: while that cell
held both arrays every in-place write copied the 4 MB field. The second
version, one return at the very end, still cost 2.4 s -- in the GATHER, where
12-22k pushes each copied the ~700 KB worklist. The push read the free slot
and the list head and then wrote, in one function. The rule this project
already had (G21/G67) is sharper than it was written: a NativeArray read
inside the function that goes on to write the array -- or that hands it to a
writer -- holds the refcount up until that function returns, and the write
copies; a read in a leaf callee that only reads is released on return and
costs nothing. Every read of the pool and of the light field in the walk is
now a leaf (`wl_head`, `wl_entry_index`, `at_level`, `give_level`), and the
gather went from 2.7 s to 0.1-0.4 ms.

**The ground readout** is rebuilt only when a key of its inputs moves (the
target column, its species, its vigour to the byte); the string was built
every frame the crosshair rested on a block. The allocation gauge did not
move (132 per frame), so that string was not what the gauge counts either.

Budget on the loaded machine: 13.3 ms best of 3 against 16 (13.9 before the
pass, same load). fps in the pinned scenario 224 against 207-209.


## Micro-voxel models (2026-09-05)

`docs/superpowers/specs/2026-09-05-micro-voxel-models-design.md`, plan
`docs/superpowers/plans/2026-09-05-micro-voxel-models.md`. Small mushrooms,
palm fronds and bush leaves are drawn as 8x8x8 sculptures of coloured
sub-cubes inside their cell: `CubeForge.Model` builds each shape as a grid of
palette indices, greedy-meshes it once into a template, and the templates ride
in the `World` beside the shown species; the foliage mesher skips model blocks
in its greedy pass and stamps their templates afterward, translated to the cell
and shaded from the cell's own light. Colours come from one palette texture
layer (89), so no shader or vertex-format change.

- **Twenty templates**, faces under caps of 160 / 120 / 100 (mushroom / frond /
  bush), asserted by `model_test`. Frond orientation reads the neighbours the
  mesher already fetches: away from an adjacent palm log, else away from
  adjacent fronds, else an umbrella over a log below, else a tuft.
- **Foliage vertices, seed 7, world start:** 47,166 before -> 142,014 after
  (3.0x; total 234,570 -> 328,000). All of it is bushes -- wild fungus has not
  fruited at frame 0. The first bush (three 2x3 plates on stems) read as little
  tables at a distance; the mound (four layers, half-widths 1, 2, 1, 0) reads as
  a bush and costs this; a five-layer mound was 171,222 (3.6x) and looked no
  better. Startup meshing 244 -> 270 ms.
- **Frame budget** (`scratch/frame_budget.sh 16 400 3`): worst frame **10.63 ms**
  after against 11.62 ms before on the same machine minutes apart -- no
  movement; the deferred and full-rebuild meshes still agree.
- **Frames:** `docs/models-mushrooms.png` -- two Meadowbell bodies on their
  mycelium at seed 7 (`CF_WILD=0 CF_AUTOPLANT=100 CF_AUTOPLANT_SPECIES=3
  CF_AUTOPLANT_MATURE=1 CF_FRUIT_RATE=2000 CF_PITCH=-140`, frame 850);
  `docs/models-bushes.png` -- bush mounds on the seed 199 oasis grass. No frame
  shows a palm: the oasis run grew none in view. Fronds are covered by the
  mesher tests (orientation on a stamped palm, and the crown stamped as
  templates rather than cubes).
- **Language notes:** an alias cannot stand in a type position
  (`B.F32Buf` fails, `CubeForge.F32Buf.F32Buf` works); a `doc` before a `type`
  is a parse error; a type named `Set` collides with the stdlib's and reports
  "expected CubeForge.Model.Set but got Set" -- renamed `Templates`; the
  alias `M` is taken by `CubeForge.Math.Mat4` across the test binary.
- 378 tests (365 before).

## Perf pass: the drain, the mesher, the relight's edges (2026-09-05)

Method: a `slow frame` line under `CF_AUTOFLOW` names every frame over 4 ms
with its phase of the period; a `drain chunk` line times each chunk's remesh
and upload; `CF_VEG_LOG=1` splits a tree edit into blocks / relight /
occupancy / rescan; and `sample` on the release binary (outside the sandbox,
`-mayDie`) gave call graphs -- once on the pinned scenario, twice on
`CF_WORKERS=1 CF_MESH_REPS=80`, a serial mesher loop that is pure mesher.
The mesh hash on the `state:` line at frame 30 of the pinned scenario
(`380281180`) was the oracle for every mesher change: it did not move once.

**What the frame looked like.** The worst frames were the ODD phases at 5-9 ms:
the mesh drain, four sections a frame, with an opaque section at 0.6-1.0 ms.
Water sections were 0.05 ms; the cost was retexturing and fungus migration
owing ~10 opaque sections a period. Phase 6 (a tree or body) was 6-9 ms.

**Where a section's time went** (serial mesher profile): the allocator. Every
Float in March is a heap object; a rectangle was some sixty of them across
quad_sized, pack_shade, layer_for (a chain of boxed literal returns, 15% by
itself) -- `march_alloc` + `march_alloc_float` + `decrc` + free were ~60% of
mesh time. The six-direction mask fill I rewrote first was a minor term.

| change | measure | before | after |
|---|---|---|---|
| per-phase drain budgets (3 odd / 2 retexture / 1 water, field / 0 veg) | worst frame | 10.6 ms | 10.1 |
| one-pass mask fill (six directions from one cell walk) | opaque section | 0.7 ms | ~0.6 (noise) |
| `F32Buf.push_vertex`: one cell rebuild per vertex, not nine | serial mesh-all x3 | 788-899 ms | (small) |
| `cf_mesh_quad`: the rectangle written in C from ints + key + layer table | opaque section | 0.6 | 0.45 |
| `cf_mesh_slice`: the whole per-slice greedy merge in C, mask read only | opaque section | 0.45 | **0.22-0.29** |
| `F32Buf.grow` as one blit; section buffers start at 16k floats | serial mesh-all x3 | 788-899 | **398-400 ms** |
| the same | startup mesh all, 64 chunks | 270 ms | **197 ms** |
| `cf_gfx_sync_box`: one texture box + exact coarse recount | tree edit, occupancy | 0.6 ms | **0.02** |
| staged upload: sixteen `glBufferSubData` become one | drain uploads over 1 ms | 7-8 of 44 | **0 of 44** |
| double-buffered mesh VBOs, capacity kept | (no measurable change on its own; kept for the staging) | | |
| `cf_u8_zero_box` / `cf_mark_box`: the relight's clear and section diff | tree relight | 2-5 ms | 2-5 ms (no change) |
| **frame budget** (`frame_budget.sh 16 400 3`) | worst frame | **10.6-11.6 ms** | **8.2 ms** |

Two things that did not pay: the relight's box clear and diff in C (the
March versions were not where the relight's time is -- the sweep and the
seed are), and GL_DYNAMIC_DRAW / double buffering for the upload stalls (the
stall was per call; staging fixed it, the buffers stayed).

**Layout notes for the shim work.** An extern's borrowed array arrives with
rc 2 (the borrow itself), so a "write through a borrowed reference" guard of
rc == 1 trips; cf_mesh_slice consumes and returns the buffer and hands the
count back in the reserved slot past the worst-case region. The relight's
field is the World's and was silently copy-on-write on its first byte; the
copy is explicit now (`copy_prefix(la, volume())`) so the shim's clear can
insist on a unique field, at the cost it always had.

**Left on the table, measured.** A tree edit is now: blocks 0.8-1.3 ms
(Veg.plant: a 64 KB chunk copy per `set_block`, ~40 of them, and a chunk
lookup per candidate cell), relight 2-5 ms (the sweep and the seed), rescan
0.3. And a finding that touches everything: `Array.PVec.get` walks a 32-long
list at the leaf and computes the tail's length by walking it, so a chunk
lookup on a 64-chunk world is ~50 pointer hops (`Array.lst_nth` in every
profile, 2% of the frame); `World.block_at` pays it per voxel in the relight's
`give_level`. GAPS G82. Phase 6 is the worst frame now: 6.1 ms mean, 8.6 max.

432 tests. `CF_VEG_LOG` and the `slow frame` / `drain chunk` lines stay as
diagnostics; the shim additions are `cf_f32_stamp`, `cf_mesh_quad`,
`cf_mesh_slice`, `cf_u8_zero_box`, `cf_mark_box`, `cf_gfx_sync_box`, all under
the blit's rc == 1 contract.

## G82 followed up: the world's chunks in a binary tree (2026-09-05)

`probes/pvec_get` timed the stdlib vector against a complete binary tree of
64 leaves, one million gets and a hundred thousand sets each, release build:

| | `Array.PVec` | `CubeForge.Tree` |
|---|---|---|
| get, indices spread | 112 ns | 72 ns |
| get, index 0 / index 63 | 61 / 154 ns | |
| set | 860 ns | 200 ns |

Per call the gap is modest; the volume is not. `World.chunk_at` is a `get`
and `World.set_block` a `set`, and the relight, the water scan, the biome's
water flags, vegetation and fruit all go through `World.block_at` a voxel at a
time. The world now keeps its chunks in `CubeForge.Tree` (six matches to a
chunk, six node allocations to replace one; `World.chunks` converts to a PVec
for the save format). Same seed, same pinned scenario, mesh hash unchanged:

| | before | after |
|---|---|---|
| tree edit, relight | 2.3-5.8 ms | **1.8-3.4 ms** |
| tree edit, blocks | 0.8-1.3 ms | 0.7-0.9 ms |
| biome field build at startup | 228 ms | **123 ms** |
| skylight + block-light flood at startup | 150 ms | 141 ms |
| startup mesh all | 197 ms | 185 ms |
| **frame budget, worst frame** | 8.2 ms | **5.7-6.6 ms** (two runs) |

The sampler had put `Array.lst_nth` at 2% of the frame. That was the top of
the stack only: the rest of a `get` -- `trie_get`, `get` itself, the tail
length walk, and the cache misses a 50-hop list walk means -- did not show
under one name. A structure change the profile rated at 2% took a third off
the worst frame. 438 tests.
## The relight box, and where the frame's memory goes (2026-09-05)

**The relight box is sized to the light around the edit.** `Light.reach`:
the brightest level at the edit voxel or beside it before the edit, plus the
new block's emission, capped at 15. Light lost by placing a block was at most
the voxel's own level; light gained by breaking one is at most a neighbour's
level less one, and an opening sky shaft shows as the voxel above at 15. A
level L propagates L - 1 steps, so a box of radius L holds every voxel that
can move, and the clear, the seed, the before-copy, the section diff and the
sweep are all passes over that box. Surface edits in daylight still get 15;
a block-light edit beside mycelium glowing at 3 gets a 5-wide box instead of
33. Oracle tests for a dim gallery and for a block-light edit beside dim
mycelium added; all "incremental equals full flood" tests pass.

| | before | after |
|---|---|---|
| a fruit body's slot (stamp, both relights, occupancy, rescan), median of 40 | ~6 ms | **3.8 ms** |
| budget | 10.25 ms | 10.03 ms best of 3 |

**The 139 live objects a frame are a leak, and it is large.** `cf_rss_bytes`
(the shim, from `task_info`) gives the gauge a byte view: the process grows
~1.8 MB a frame, 5.7 GB resident after 2,400 frames, identically on the old
toolchain pin. Stage probes on a frame: the water tick retains 4.6 MB per
tick, the biome slice ~350 KB every frame, the mycelium tick 480 KB, the
drain ~100 KB; every stage that replaces part of the world leaves the old
part alive. The stack pointer does not move between frames, so the loop is a
true tail call, and making its parameters owned through identity functions
changed nothing. `MARCH_TRACE_GC=1`'s allocation log (25 GB for 140 frames)
gives the survivors: 64 KB chunk arrays at 29 a frame, 4 MB light fields,
0.5 MB relight prefix copies, the field arrays, and thousands of 32-byte
list cells. Three compiler-side causes, each with a repro:

1. **A library-defined type gets no deep drop (GAPS G79).** Type definitions
   are registered under qualified names; use sites carry the short name;
   `Repr.find_variant` is exact. The drop pass found no constructors for
   essentially every library type, freed each dying cell shallowly and leaked
   its children. `probes/drop_xmod` (WHICH=1,2): 2.1 GB -> 8 MB with the fix
   on the March branch `fix/drop-short-type-names` (checkout under the
   session scratchpad). On this project it freed the field arrays (16 KB
   survivors 811 -> 247, 131 KB 392 -> 106) but not the chunks.
2. **A closure environment is freed shallowly (G80).** Every captured value
   leaks. `probes/drop_xmod` WHICH=5: a thousand closures capturing 1 MB each,
   called once and dropped, leave 1.07 GB resident. This is the chunk leak:
   `Array.set`'s two update paths capture the new element in a closure, so
   every persistent-vector update in every March program leaks the element.
   WHICH=4 (a 64-element vector, 4,000 replacements of 64 KB arrays): 339 MB
   resident against 4 MB live. Needs a per-closure-type drop or a runtime
   release that knows the capture layout; not attempted here.
3. **A named binding unused in one arm of a lifted closure is never released
   (G81).** `Array.set`'s `lst_set` bound the replaced element as `h` and left
   it unused in the replacing arm; the IR has no release for it, where a
   wildcard gets one. Fixed in the stdlib on the same March branch by matching
   with `_` in that arm; the probe still leaks through (2).

Until the toolchain carries those fixes the project pins March main
unchanged (`watch-9ca8a98d`). Two project-side releases added on the way,
`Biome.release` and `Myc.release`, destructure a replaced field so its arrays
die as bindings; harmless with the fix, and they cover (1) for those two
types without it. The four colliding short type names (Field, Relit, Felled,
Sweep across modules) were renamed unique; a collision also forces Boxed in
the compiler and is worth avoiding regardless.



## The relight reads opacity from the occupancy field (2026-09-05)

The sweep's remaining cost was `World.block_at` per neighbour, to learn
whether the neighbour is air, water (opacity 2), leaves (6) or opaque. The
occupancy field already held a byte per voxel for the shadow texture; it now
holds `Light.occ_value`: 255 where the block is opaque (what the shader, the
AO and the coarse counts read as solid, unchanged), else the block's opacity.
The sweep reads that byte and never fetches the block.

What made it correct: the field has to be complete. Two attempts said so.
The first wrote the finer byte only where `set_occupied` was called and read
water as air wherever the water actors had applied cells or leaf decay had
run -- the relight-equals-full-flood tests failed and the mesh hash moved. The
second used the byte only for solid neighbours and gained nothing: the reads
are on air and water. So every block write now keeps the byte -- there are
exactly two writers, `World.set_block` and `World.set_cells`, and
`set_occupied` is gone -- and every World constructor builds the field, so a
relight never runs against a stale one.

| | before | after |
|---|---|---|
| tree edit, relight | 1.8-3.4 ms | **1.3-2.2 ms** |
| frame budget, worst frame | 5.7-6.6 ms | **5.75 ms** |
| mesh hash at frame 30 | 380281180 | 380281180 |

A side effect worth knowing: a bush edit now pays ~0.3 ms it did not before.
Its leaves were written with `set_block` alone and never touched the
occupancy field; now every edit's first write to that shared 4 MB field is a
copy-on-write. Trees paid it already through `set_occupied`.

Not kept from this stretch: a VAO per mesh slot (six alternating runs were
noise, so 192 attribute-pointer sets a frame are not where a quiet frame's
time is on this driver). Kept: the shim's `getenv("CF_DEBUG")` on every draw
call is now read once.

## Tree placement in one write; the copy-on-write that remains (2026-09-05)

`World.set_blocks` takes a list of packed cells, splits them by chunk and
applies each chunk's cells through `set_cells`: one chunk copy per chunk
touched, one pass over the occupancy bytes. `Veg.plant`, `Veg.fell`,
`Fruit.stamp` and `Fruit.fell` gather their cells against the world as it
stands and write once, where each used to call `set_block` per block and copy
the 64 KB chunk every time.

| | before | after |
|---|---|---|
| tree edit, blocks | 0.8-0.9 ms | **0.3-0.45 ms** |
| a fruit body | 4.2 ms | **~2.0 ms** |
| bush edit, blocks | 0.3 ms | 0.3 ms |

The bush number is the finding. A bush is nine cells, and with the occupancy
write skipped as an experiment its blocks cost 0.02 ms: the 0.3 ms is the
first byte written into the World's 4 MB occupancy field copying it, because
the field is shared at that moment. `Win.arr_rc` (a new diagnostic, the
array's refcount word as the shim sees it) reads 2 for the occupancy field and
5-8 for the light field at the start of a tree edit. Moving the vegetation
branch into a helper so the old scene is not named in an else arm changed
nothing. The extra reference is somewhere else in the frame; the same copy is
what `relight_marked` pays explicitly for the light field. Left open, with
the diagnostic in place.

Hash at frame 30 unchanged through all of it (103332325 since main's
relight-box merge). 440 tests.


## Pinned to March main 7eb8d76a (2026-09-05)

`watch-7eb8d76a`: March main after the two fixes from the leak work (GAPS G79
and G81) were merged, built with `make install PREFIX=...` plus the `stdlib`
symlink, March's own 706 tests green. On this project: 440 tests, budget
6.47 ms best of 3, the allocation gauge **139 -> 86 objects a frame**. What
remains is G80, closure captures never released, open in March.


## The light oracle: what the mesh hash was saying (2026-09-05)

The frame-30 mesh hash moved on main's relight-box commit (380281180 ->
103332325). Every change of this pass had held it, so the question was
whether main's relight was wrong. The dump frame now prints a **light oracle**:
both light fields against a flood from scratch, as counts of differing voxels
and the first few with their kept/flooded values. Bisected with the same
oracle patch on three builds, seed 7, frames 30 and 300:

| build | sky | block |
|---|---|---|
| before both changes (14fdab3) | 18 / 43 | 0 / 0 |
| this pass's occupancy-byte sweep alone (33a555c) | 18 / 43 | 0 / 0 |
| main's reach-sized box alone (e7029a6) | 44 / 88 | 0 / 19 |

So the occupancy-byte sweep is exact, and main's commit did introduce wrong
light -- but the 18 and 43 were already there. Two causes, both found:

1. **Water moves without a relight.** Every voxel in the base count is kept at
   15 where a fresh flood says 13, at y 63-83 -- water (opacity 2) that the
   actors moved through `set_cells` after the flood. Nine voxels by frame 3,
   before any other edit. Deliberate: a region relight per changed section
   would be tens of milliseconds a tick for a one- or two-level shade under
   moving water. Recorded as an accepted approximation in `todos.md`.
2. **Multi-block edits were relit from one voxel.** A tree, a fruit body and
   a batch of mycelium each called the relight at their centre, so a box of
   radius 15 round the trunk missed the shade a canopy casts four columns out
   (visible as kept 14 against 15 from frame 6, the first tree), and with
   main's reach-sized box the mycelium batch's window -- taken within 4 of an
   anchor on the promise of a radius-15 box -- got a box of the anchor's own
   dim light and left emitters outside it: the 19 block-light voxels.

`Light.relight_region_marked` / `relight_block_region_marked` take the edited
region and grow it by a radius; `World.relight_region_marked` uses
`Light.region_reach` -- the brightest light in and round the region before
the edit, or the brightest emission in it after -- so a tree in daylight gets
15 and a glowing patch in a dark wood gets its glow. Trees, bushes, bodies
and the mycelium anchor use it; a single block still uses `reach` at the
voxel. A fixed radius 15 for regions was tried first and put the worst frame
at 11.3 ms through the mycelium batch's 39-wide box; the reach brought it
back.

| | before | after |
|---|---|---|
| light oracle, frame 30 / 300 | sky 44 / 88, block 0 / 19 | **sky 18 / 42 (all water), block 0 / 0** |
| tree edit, relight | 1.3-2.2 ms (wrong box) | 1.7-2.9 ms |
| frame budget, worst frame | 5.7-6.7 | 6.46 ms |

A new test plants a tree and checks the region relight against a full flood.
441 tests. The oracle stays in the dump: a non-zero block count, or a sky
count whose first voxels are not the 15/13 water pattern, is a relight bug.

## Eating from the inventory (2026-09-06)

Spec `docs/superpowers/specs/2026-09-06-eating-from-the-inventory-design.md`.
A cap has been food since the effects work, but the only way to eat one was to
hold it in the selected hotbar slot, aim at **nothing**, and right-click. That
gesture is gone. Two deliberate ones replace it:

- **E** eats the selected hotbar slot, whatever the player is aiming at, and
  is inert while the inventory window or the escape menu is up.
- **A right-click on any slot** while the inventory window is open eats that
  slot -- hotbar or backpack. Left-click keeps drag and drop, and right-click
  did nothing in the window before, since `interact` never runs while a panel
  is open. This is the half that matters: caps pile up in the backpack, and
  they used to have to be dragged into the hotbar before they could be eaten.

Two functions carry the rules, so the gestures cannot disagree and both are
unit-testable without a window. `Inventory.edible(id)` is the only answer to
what is food (caps, and nothing else). `Inventory.eat_slot(window_open,
ui_open, right_click, hovered, eat_key, sel)` is the only answer to which slot
a bite addresses, or -1; whether that slot *holds* food is deliberately not its
question, so an inedible slot is a no-op rather than a refused gesture. The
frame loop reads both and calls `eat_at`, which consumes one through the new
`Inventory.consume_at` (consume was hard-wired to the selected hotbar slot) and
applies the species effect as before -- thirty seconds, refreshing.

`CF_AUTOEAT=<frame>` used to call the bite directly and so tested nothing about
the gesture. It now puts a Frostcap cap in the inventory and presses the key
thirty frames later; `CF_AUTOEAT_SLOT=<slot>` puts the cap in that slot and
eats from there instead. Both print `ate a Frostcap cap: JUMP 30` and the dump
shows `effects: JUMP 30`, from the hotbar and from backpack slot 20.

448 tests (441 before): what is edible, which slot each gesture addresses,
consuming from a given slot, and the last one emptying it. Mesh hash, light
oracle and frame budget unmoved (380281180, sky 18 block 0, 6.42 ms).

The one thing not covered headless is the mouse itself: the scripted knob
supplies the slot, so the click-to-slot rule is tested through `eat_slot`
rather than through a real right-click over a real cursor position.

## The mycelium skin, dialled back (2026-09-06)

Mycelium showed too strongly: on grassland the ground read as a change of
biome rather than a skin over one. `Texture.myc_tint()` is one dial over both
halves of the texel formula -- how many texels are threads, and how far a
texel is pulled toward the species colour -- as twelfths, so 100 is exactly
the old fractions (a third of texels, two-thirds species colour on a thread,
a sixth elsewhere) and 0 is the bare surface, byte for byte. **The default is
now 60.** `CF_MYC_TINT` overrides it, which is how the comparison below was
rendered from one build.

Judged on a mature patch on open grassland (seed 11, `CF_PITCH=-115`), at 100,
60, 35 and 18, against a control with no fungus: `docs/fungus-tint.png`. At 60
the ground keeps its own green with a warm cast; at 35 it is nearly plain
grass; 18 is indistinguishable at a glance.

Two things the comparison settled that guessing would not have:

- **A glowing species' loudness is mostly its light, not its texture.** The
  first ladder used Lanterncap, whose surface mycelium emits 6, and the
  panels differed as much in banding as in colour. Repeating it with
  Meadowbell, which does not glow, isolated the dial. The glow is deliberately
  untouched: lit ground at night is what glowing fungus is for.
- **The first scene was worthless and looked fine.** Planting at the seed-7
  spawn now lands in a grove, where the surface is bush leaves: leaves carry
  no mycelium, so `wanted 4 shown 0` and four tint settings rendered four
  identical frames. The readout line at the dumped column is what caught it.

Verification note: a raw `cmp` of two frame dumps always differs, because the
FPS counter is drawn into the frame. `scratch/cmpframe.py` masks it, and by
that measure two identical runs match exactly (0 differing pixels) and the
shipped default matches an explicit `CF_MYC_TINT=60` (0), against 1.79M
pixels differing from 100. Chunk streaming did not cost reproducibility.

458 tests (three new ones pin the dial: 0 is the bare base, the default shows
but less than 100, and the thread count thins as it comes down), lint clean,
budget 6.44 ms.


## The black bands under glowing fungus were a vertex-interpolation bug (2026-09-06)

Reported as "those fungal stripes shouldn't be black". They were not the
texture: a mycelium layer's darkest texel is its base's darkest texel, and
fungus only ever lightens it (grass 70,140,60; Lanterncap over grass at 60,
85,145,61). The bands were the sky term of the lighting, destroyed in transit.

Sky and block light shared one vertex float: `2 * round(blk * 255) + sky`,
unpacked in the fragment shader with `floor` and a subtraction. A varying is
interpolated across the triangle, and that unpack is not linear, so between
two corners that are **both fully sunlit** but differ in block light the
decoded sky ran

    1.0  1.4  1.8  0.2  0.6  1.0  1.4  1.8  0.2  0.6  1.0

a sawtooth. The troughs are the black bands, and the peaks above 1 are the
blown-out bright bands beside them. It appeared only where a block glowed,
which is why fungus wore the blame: with a non-glowing species the same
ground rendered flat and clean, and with a glowing one it looked terraced.

**Fix: the two channels are separate attributes.** The vertex goes 9 floats to
10 (pos.xyz, uv, layer, sky, face, fx, block light) and the packed word is
split on the CPU -- in `F32Buf.push_vertex` and the shim's `cf_vert` and
`cf_f32_stamp` -- so it never reaches a varying. Two channels cannot share one
interpolated scalar; no encoding fixes that, because interpolation is linear
and any unpack is not.

`docs/fungus-blockband.png` is the same patch before and after. At midnight the
glow still does its job: ground luminance median 33 under a glowing species
against 9 under a non-glowing one, and even, with no banding.

Two things the vertex growing from 9 to 10 turned up, both silent until they
were not:

- **`cf_f32_stamp` had `n % 9` and `i += 9` of its own**, so every model
  template (mushrooms, fronds, bushes) aborted the moment the vertex grew.
- **Floats-per-quad was the literal `54` in four places**, twice in
  `F32Buf.push_quad`/`push_slice` and twice in the shim's `cf_mesh_slice`,
  which reserve and write the same buffer. They drifted apart and quads went
  missing (a six-quad slab meshed as five). Both sides now say it once:
  `F32Buf.quad_floats()` and `CF_QUAD_FLOATS`.

460 tests, lint clean, budget 6.61 ms with the 11% wider vertex. Two new tests:
one asserts the old packing is *not* interpolation-safe (both corners decode to
sky 1.0, three tenths of the way across it decodes to 0.2), the other that
`push_vertex` lands sky in slot 6 and block light in slot 9. A point test of
the encode/decode passed throughout and could never have caught this -- the bug
lives between the vertices, not at them.


## Branching filaments, drawn in the shader (2026-09-06)

The tile gives mycelium a warm cast; this gives it threads. They are drawn in
the fragment shader, not baked into the texture, for one reason: **a tile
cannot branch across a block boundary.** Sixteen edge-connection variants per
(base, species) would be 672 layers against the atlas's 92, and the threads
would still repeat every block. Fed world-space coordinates instead, a
filament crosses from block to block unbroken and the pattern never tiles.

`hyphae(p)` is ridged, domain-warped value noise: the ridge is where the noise
crosses its midpoint, and that line wanders and forks, which is what reads as
branching. Two octaves, one warp, sixteen `hash12` calls per mycelium
fragment.

The shader needs no new vertex data. The layer index already carries the
species -- mycelium layers run `myc_first + 6 * base + (species - 1)` -- so
`(layer - first) % 6` is the species, and the six colours arrive once at
startup in a uniform, from `Species.colour_*`, the same source the map overlay
and the tiles use. Guarded on `u_unlit == 0 && u_use_tex == 1`, so overlays and
the map are untouched.

Defaults `Species.branch_strength` 60, `branch_scale` 500, `branch_sharp` 93,
each overridable (`CF_MYC_BRANCH`, `CF_MYC_BRANCH_SCALE`, `CF_MYC_BRANCH_SHARP`)
-- the settings were chosen by sweeping them from one build. Scale is the
knob that matters: at 100 the threads were blurred smudges several blocks
wide; from about 450 up they read as filaments.
`docs/fungus-filaments.png` is the tile alone against the tile with threads.

Cost is under the noise floor. Wild world, 600 frames: 284 fps with, 267
without. Standing in a patch that fills the screen: 269 with, 259 without --
the "with" runs measured faster both times, which is how much of a difference
there is to find. Budget 6.32 ms, 460 tests, lint clean.

## Animals: bodies that read as animals

Slice 1 of the fauna work: a transform stamp, body-plan generators, and the
first two species.

**The stamp.** `cf_f32_stamp` translates a greedy-meshed template into a cell;
an animal also needs to turn and to come in sizes. Baking yaws was the obvious
move — palm fronds already bake eight directions — and it is the wrong one
twice over: eight steps snap under a banking bird, and `species x poses x yaws`
multiplies the template table by an order of magnitude. `cf_f32_stamp_xf` adds
a yaw and a uniform scale to the same copy loop: two multiply-adds per vertex
on a loop that is already memory-bound. The scale term is what makes "fish of
multiple sizes" a column in a table rather than a second set of grids.

Face indices rotate with the body, by the nearest quarter turn. Without that a
turned animal keeps the lighting it had facing east.

**Bodies are generated, not drawn.** A species gets a row — how long, how tall,
how far the wings reach, whether it flies — and one of two generators builds
every pose from it, placing each part against the body's own extent. The first
hand-built bird had its wings two cells clear of its flank, touching nothing,
and rendered as a bird with two slabs floating beside it.

The invariant is a test, not a hope: a flood fill from one cell must reach every
filled cell of every pose. Adjacency alone would not do — the broken wing was a
solid slab whose own cells touched each other perfectly well; only reaching
every cell from a single seed catches a part that is whole and in the wrong
place. It has caught three real defects since: a wing tip stepping in z and y at
once (meeting the wing along an edge, no shared face), a fin hung off the row
the body-rounding shave removes, and a tail-sweep connector whose z bounds
arrived descending, which `box_n` counts as an empty range and silently skips.

**16 cells a side, not 8.** Every animal at 8 read as a stack of slabs, and the
reason is structural: an eighth of a block is the thinnest thing that exists, so
a wing, a fin and a beak all weigh as much as the body. `Model`'s greedy pass is
now parameterised on the grid side; the 8-cube path is unchanged and the world
mesh hash is byte-identical across the refactor (413448066).

**What it costs.** Interleaved A/B, three runs each, 150 bodies stamped and
uploaded every frame:

| bodies | worst frame | frame rate |
|---|---|---|
| none | 5.4-6.6 ms | 214-225 fps |
| 150 at 8 a side | 6.7-7.0 ms | 207-218 fps |
| 150 at 16 a side | 6.6-8.0 ms | 189-208 fps |

At 8 the bodies are lost in the noise; at 16 they cost about a millisecond and a
tenth of the frame rate. Worth paying, and worth knowing: the visible cap is now
a real budget rather than a formality. This is also the first system whose cost
is per FRAME rather than per tick, so none of the phase-slot spreading that
carried the world tick from 55 ms to 11 applies to it.

`CF_FAUNA_DEMO=1` circles a flock and a school on the clock; `=2` stands every
species in every pose, still and broadside, which is the only way the bodies
themselves are actually inspectable — at their real size, 0.35 of a block, an
animal is a few pixels and a screenshot proves only that something was drawn.

## Flocks: a group as one unit of state

Slice 2: the flock actor, its steering, and the interpolation that lets it
think six times a second and still move smoothly.

**One actor per flock, not per animal.** The literal reading of "animals are
actors" costs a call/reply pair per animal per tick against a 16 ms frame, and
then needs actors talking to each other to do what one shared state does for
free. A `FlockActor` owns 5-20 animals; `Flock.step` is an ordinary function
over them and the actor is a shell, which is the division `Water` already uses
between `tick` and `WaterChunk` — and it is why every behaviour here is tested
without a running actor.

**The flock is told the terrain; it does not regenerate it.** `WaterChunk`
answers the no-arrays-in-messages rule (G44) by rebuilding its chunk from
`(cx, cz, seed)`. Copying that would have broken the feature outright:
regenerated terrain is terrain as it was *generated*, and the point of the
fauna design is that the player has changed it. A flock gets a 5x5 patch of
ground heights packed a byte at a time into four Ints, and is therefore correct
against edits by construction.

**Two corrections the build made to the plan.**

The inputs cannot ride on the call request. `Actor.call(pid, Req(a, b, c))`
delivers zeros — silently, with no error anywhere (GAPS G84). The flocks read a
ground of 0 and a water surface of 0, so the fish sank to y 0.5 (its floor,
ground + 0.5) and the birds set off toward y 9. It was caught only because
y 0.5 under a lake at y 81 is too specific a number to be anything but
arithmetic on a zero. Inputs now go by `send`; the call is nullary.

"Ground" turned out to be two questions. `Biome.height_of` is the surface
*including* water — a sea column reads 62, the water top — which is right for a
bird and exactly wrong for a fish, whose floor would then sit above its own
ceiling and push it out through the surface. Fish read the bed via `surface_y`
instead. Bounding that walk to start at the field's height rather than at y 255
matters: unbounded, the flock slot was the most frequent slow frame in a
400-frame run (12 occurrences, more than the vegetation or climate ticks);
bounded, it left the list entirely.

**What it costs.** Interleaved A/B, three runs each, four flocks and 29 animals:

| | worst frame | frame rate |
|---|---|---|
| no flocks | 6.5-7.2 ms | 217-218 fps |
| four flocks, 29 animals | 6.5-8.1 ms | 205-218 fps |

Within noise once the bed walk is bounded. The AI runs on its own phase slot,
once per world tick, and the renderer blends the last two ticks — so the
steering runs at a sixth of the frame rate and the animals still move smoothly.
That is the trade the biome sweep already makes, and here it is the only one
available: unlike every other tick in this program, the DRAW cannot be spread
across frames.

`CF_FAUNA_DEMO=3` puts three flocks of pipits around the player and a school of
sunfin in the nearest water — which for seed 7 is an upland lake at y 81, not
the sea, and so a decent test of the case the sea-level assumption would have
got wrong.

## The roster: eighteen species from a table

Slice 3. The species table is now *generated* from a data file rather than
hand-written: eighteen rows produce the March if-chains for names, activity,
size, niche, six plan numbers and a colour. Hand-editing a dozen chains to
insert a species in the middle is how off-by-one `end` counts get in, and one
extra `end` silently closes the module — the parse error points at whatever
follows, not at the chain.

**Finer voxels, and what they cost.** 16 cells a side fixed the slabs; 32 gave
a head enough cells to round; 64 is what makes a wing one cell against a body of
sixty. Naively that is 8x the greedy work of 32, and it showed: startup went
from 1.7 s to 4.2 s.

The fix is that a body fills a fraction of its grid. The greedy pass sweeps 6n
slices of n x n mask cells, and at 64 most of those slices are past the animal's
nose or beyond its wingtips. Finding the bounding box once and keeping both the
fill and the walk inside it took the fauna templates from 2.4 s to ~0.3 s —
startup 2.3 s against a 1.7-2.1 s baseline. The block models go through the same
parameterised code and their mesh hash is byte-identical (413448066), which is
the only reason a refactor of the mesher was safe to make at all.

**Size range.** 0.22 blocks (Cinderfinch) to 4.20 (Mossmoa), about twenty to
one. The small end has to stay small for the large end to mean anything.

**Three shapes that are branches, not new code.** A penguin is `plan_upright`:
the head goes on top of the body instead of in front of it, and the pale
underside becomes a pale front. It already had short wings, legs and a dark back
from ordinary rows. A shark is `plan_dorsal` at 8 with a swept leading edge — a
rectangle of the same height reads as a sail. An eel is `plan_pectorals` false,
which is most of why an eel reads as an eel.

**The connectivity oracle earned its keep twice more.** A penguin's legs stopped
at `body_y0 - cells(2)`, relying on the belly to bridge to the body — and an
upright bird has a front instead of a belly, so its legs hung in the air. And
the wing taper's two ends crossed on a short body with a long span, which
`box_n` treated as an empty range and silently skipped: three birds lost their
wings entirely and still built clean.

That was the third time an inverted range vanished, so `box_n` now **sorts** its
bounds. A caller handing over a reversed range means the box between the two,
and gets it.

The test itself had to change twice as the grid grew: repeated sweeps until the
marked count settles is O(cells x rounds), which is fine at 16 and hopeless at
64 x 54 templates. It is a worklist flood now — pop, mark, push the six
neighbours — and the suite runs in about 90 s.

**What it costs.** Live flocks against no flocks, interleaved, three runs each:
worst frame 7.4-8.0 ms against 6.5-7.3, frame rate within noise. The frame
budget gate is 6.71 ms against 16.

## Detail, not resolution — and then resolution where it shows

The question was whether to spend polygons on detail or take the voxels down
another size. Measured first, because the intuition is unreliable:

| grid | quads over 54 bodies | largest body |
|---|---|---|
| 32 | 7,253 | 183 |
| 64 | 10,131 | 301 |

Resolution does add some quads -- features pinned at one cell (a wing's
thickness, an eye) get relatively finer -- but the number that decided it was
**301 quads for the largest body**, about 600 triangles. An order of magnitude
under what the frame carries. The bodies read as boxes because the generator
only knew how to draw boxes.

So the detail went in first: a body built a SLICE at a time with a taper along
its length, a head built the same way, a notched tail fan, feet. Then the change
that mattered most -- a **rounded cross-section**. The taper narrows a body along
its length and does nothing at all for the angle you actually meet an animal
from; head on, a stack of boxes is a rectangle. Rows taken from an ellipse fixed
that, and it is the single biggest visual change in the whole feature.

Then resolution, but **per species**. A template is normalised to a unit cube
whatever grid built it, so different species can be built on different grids at
no downstream cost: 128 for the five big ones, 64 for the middling, 32 for the
small. That is not a compromise, it is the measurement -- a Cinderfinch is 0.22
blocks, so one of its 64 cells is already about a screen pixel at five blocks,
and halving it again buys a subdivision nobody can resolve while paying for it
on the commonest animal in the world. A Mossmoa is four blocks and one of its
cells is seventeen pixels.

| | |
|---|---|
| quads over 54 bodies | 43,869 |
| largest body | 2,524 (Deepmaw, on its 128 grid) |
| startup | 3.5 s, against 1.7-2.1 s with no fauna |
| frame budget gate | 10.5 ms against 16 |

The startup cost is the honest price of the 128 grids: about 1.4 s, and worth
knowing before it grows. Bounding-box culling in the greedy pass is already
carrying most of it -- without that, 64 alone cost 2.4 s.

**The connectivity oracle found six more defects in this pass**, every one of
which built and linted clean:

- eyes stuck to the head's side while the head had tapered in past them
- a wing whose taper crossed itself on a short body (three birds, no wings)
- a wing reading the body's surface at ITS OWN row, so when the flap's ramp
  stepped to a new row two neighbouring columns referenced different surface
  positions and the tip came off
- a neck starting at `body_y1` while the nose had tapered down below it
- legs set out to a stance, past the narrow bottom row of a rounded body
- a penguin's legs stopping where a belly would have bridged them

The lesson worth keeping is not any of those. It is that the oracle was made to
NAME what broke -- species, pose, the stray piece's size and its first cell --
after bisecting by hand cost three ninety-second runs. The diagnostic paid for
itself twice over in the same session.

## Birds and fish, redesigned from the ground up

The previous builder was a box with parts attached, and every improvement to
it -- rounding, tapering, feet -- made better parts on the wrong skeleton. The
moa that came out of it had a head the size of a fist glued to a beach ball, a
plank for a tail, and no neck at all. A bird is a SMALL HEAD ON A THIN NECK over
a teardrop; a fish is a laterally compressed loft that tapers to a peduncle.
Neither is a box, and no amount of refining a box gets there.

**Two primitives.** A LOFT is an ellipse swept along a path with its radii
following a profile: the body, the neck, the beak, a leg. A SHEET is a thin
plate: a wing, a tail fan, a fin. That is the whole vocabulary. Consecutive loft
slices overlap by construction, so a loft is connected without anyone checking;
a sheet's root is buried inside the loft it hangs off, so it cannot detach. The
connectivity oracle still runs, but on this builder it is a guard rather than a
bug-finder -- it caught two, both a notch or a sweep stepping in two axes at
once on a 32-cell grid, the same class as before.

**One profile curve.** Two half-ellipses meeting at a peak: a smooth egg, fat
where the row says and drawn to a point at both ends. With the peak at 45% it is
a bird's chest, at 35% a fish's shoulder, at 50% a penguin's belly. A fish's
width follows the square root of it, which keeps the body deep further aft --
the peduncle is most of what makes a fish look like it swims.

**The neck is a tube from the shoulder to a small ball.** Head size and neck
thickness are rows of their own now. A neck as wide as the head is not a neck;
a head sized to the body makes every bird a duck.

| | before | after |
|---|---|---|
| quads over 54 bodies | 43,869 | 83,723 |
| largest body | 2,524 | 7,160 (Mossmoa, 128 grid) |
| startup | 3.5 s | 2.8-2.9 s |
| live flocks, frame rate | within noise | -4% (132-134 vs 138-141) |
| live flocks, worst frame | +1 ms | +0.5 ms |
| frame budget gate | 10.5 ms | 5.5 ms |

The quads doubled and the startup FELL: a loft fills fewer cells than the boxes
it replaces, and the bounding-box greedy pass scales with what is filled. The
frame budget number is mostly a quieter machine, and is reported as measured.

## Populations: what lives where, and why

Slice 4, the rule that closes the terraforming loop. One count per chunk per
species, 0..15, eased one step a visit toward a CARRYING CAPACITY read off the
world: for a fish, how many of the chunk's columns are water of the depth it
wants (and of the body size -- a Sunfin wants a pond, and the field's own
`small_body` of 48 columns is what says whether it has one); for a bird, how
many columns are its biome. Both are numbers the world already maintains and
the player already changes. Dig a 6x6 pond three deep in grassland and two
Sunfin arrive over a few visits; fill it in and they go, one a tick. That is the
whole mechanism, and it is a test.

**Flocks are the visible sample of populations.** The registry counts every
chunk of the window; a flock is placed only for a species with at least two
animals in a chunk within two of the player, retired when the count goes or the
player does, and resized in place when the count moves -- a school grows with
its pond without every fish jumping back to where it began. Eight slots. In a
400-frame run on seed 7 that is 792 animals counted and 34 drawn, spanning
Pinecrest, Duskowl and Mossmoa in the forest chunk and Frostgull, Brinewaddle,
Shoalback, Kelpjaw and a Bladefin on the coast: the habitat rule doing real
work, with no species placed by hand.

**Two bugs, both instructive.** The first was `slot >= 0 && have !=
List.length(Array.get(cur, slot))` -- `&&` does not short-circuit (GAPS G33),
so the PVec was read at -1 on every chunk with no flock, and the program panicked
on the first population tick. Found by bisecting the slot's three stages with a
temporary knob; the diagnostic printlns never showed because a panic drops the
buffered stdout, which is worth remembering. The second was cost: `capacity`
walked every column's depth per species, 4,608 walks a chunk, and the frame
budget gate failed at 18.55 ms with the fauna slot the second most frequent
slow phase. Profiling each chunk ONCE -- 256 depths and 256 small-water flags --
and running the species against the profile brought the gate to 7.72 ms.

**Save/load.** The header carries a `pop` line, 278 entries for the seed-7
window at the save; a session loaded from it reports 340 animals at frame 0,
before a single tick has run. A save from before there were populations reads
an empty list and regrows.

## Populations that persist, and the first season

The dense-window registry was the honest simplification for slice 4, and it
was the wrong shape for what comes next: a bird that leaves in autumn and comes
back to the SAME lake needs the lake's record to survive the lake leaving the
window. So the registry now archives every chunk that leaves, under its world
coordinate with the tick it left at, and restores it on return -- CAUGHT UP,
each species moved toward the capacity of the world as it now is by the visits
it missed. A pond that filled while nobody was watching has its two Sunfin when
you get back; a pond filled IN while you were away has none. Both are tests.

The save carries every record, window and archive alike, keyed by world chunk:
a load at the same origin puts a school where it was, a load at another origin
archives it under world chunk (6, 4) until the window reaches it again. 1,281
integers in the seed-7 header at the save; 302 animals counted at frame 0 of
the loaded session.

**Seasons** are a capacity that is zero out of season. A year is eight days --
an hour of play a season at the default half-hour day, long enough to notice a
bird has gone and short enough to see it come back. The Frostgull holds the
coast for the second half of the year and the Marshheron the wetland for the
first. That is migration as the population sees it; the flocks crossing the sky
are next.

Frame budget 6.83 ms against 16; save/load round trip passes; 500 tests.

## The flyover

Departure is a flock mode: when a migrant's season closes, `mode_of` returns
`depart` ahead of everything else -- ahead of the player, ahead of the clock --
and the flock climbs thirty above its cruise, drops the pull home, and pushes
the way its species leaves at fleeing speed. The loop keeps its slot while the
count drains underneath it (which would otherwise free it mid-departure) and
lets it go once the lead is forty-four blocks out.

Arrival needed no behaviour at all. An arriving migrant is placed forty-four
blocks out and twenty-five up, from the way it leaves; the pull home it has
past `home_radius` and the altitude spring fly it in over ten seconds or so.
The test is the same for both: a gull whose season is over ends sixty ticks
well above where it would cruise and thirty north of home, past the reach; a
gull started forty-four out and twenty-four up ends a hundred and twenty ticks
inside its home radius, down at its cruise.

Watched in the game with a two-second day, so the year turns in sixteen: at
year 0.70 a Marshheron -- whose season closed at 0.5 -- is at y 103 seventeen
blocks north of its home, on its way out. Nothing placed it there but the
calendar.

## The survey reads the animals

Slice 5, and the line that closes the loop from the player's side. The survey
instrument that says what a place IS now also says what could LIVE there and
what to dig to get it.

Aimed at an animal -- a sphere test along the look ray over every drawn animal,
run only while the survey is open or the scan key is down -- the panel names it
and its niche once scanned (`FROSTGULL  BEACH TUNDRA  WINTER`), and prompts
`UNKNOWN ANIMAL  SCAN` until then. The scan key takes the animal when there is
one under the reticle and the ground otherwise, so the two catalogues share a
key without fighting over it. Scanned animals are a bitmask of their own in the
header, beside `known`: eighteen species beside six, and two catalogues that
read independently.

Aimed at the ground, for the reticle's chunk and only for species already
scanned: which would live here, and the first that would not and why -- in
terms the player can act on. A fish wants a depth (`NEEDS DEEPER WATER  14`),
a pond, or open water; a bird wants a biome (`PINECREST  NEEDS TAIGA FOREST`);
a migrant out of season is `AWAY UNTIL WINTER`. On the seed-7 spawn hill with
everything scanned: `WOULD LIVE HERE  GRASSPIPIT  CINDERFINCH / PINECREST
NEEDS TAIGA FOREST`. Nothing scanned, nothing said: the gate is the same one
the fungus half already uses.

The report is per CHUNK, because that is where a flock lives, while the rest
of the panel is per column; a Cinderfinch reported on a grassland column is a
desert corner of the same chunk. Honest, and slightly surprising; worth a word
in the panel if it confuses anyone.

Frame budget 8.29 ms against 16; save/load round trip passes.

## Roost

A bird asleep on the ground with its wings level looked like a bird that had
landed and forgotten to stop flying. The fourth pose folds them: a shell one
cell thick against each flank, from the shoulder back toward the tail, placed
row by row against the body's ACTUAL surface (body_half_z) so it sits on the
bird rather than beside it, narrowing toward the tail, dark at the rear where
the primaries cross. The flock keeps its mode from the last step and a still
animal wears the fold whenever that mode is roost; one that is moving -- put
up by the player -- flaps like any other. A fish's fourth pose is its first.

With it: the inner wing's trailing edge is feathered (a cell cut every fourth
column), and the ratite leg has a hock -- the joint set well back and
thickened, the shank angled forward -- which is the difference between a bend
in a post and a joint.

**One lesson.** The first build of this "worked": it compiled, linted, and the
roost render showed a bird with its wings raised. Two of the edits had silently
not applied -- a text replace on a string that no longer matched -- so pose 3
fell through the flap's bend to the "up" branch, and the fold functions were
never in the file. What caught it was a test with the wrong premise: I had
asserted a folded wing was fewer faces than a spread one, it failed, and
following that up found that there was no folded wing. The assertion is now the
true property -- a folded wing reaches under six tenths as far out as a spread
one -- and every generated edit here is now checked in the same command that
makes it.

505 tests, frame budget 8.23 ms against 16.

## The spec is built

The last line of it: fish read the clock. A twilight species (Reedcarp) holds
under a block of water at dawn and dusk and three times its ordinary depth at
midday; a night species (Glasseel, Deepmaw) comes up in the dark; a day species
does not care what time it is. One function, `fish_hold`, folded into the
height a fish wants -- and a test that a Reedcarp at dusk sits two blocks above
one at noon while a Sunfin at either hour sits within a block of itself.

And the habitat line now says what it is: `THIS CHUNK WOULD HOLD  GRASSPIPIT
CINDERFINCH`, because the report is per chunk where the rest of the panel is
per column, and a Cinderfinch reported on a grassland column was a desert
corner of the same chunk.

Frame budget 6.72 ms against 16.

## A perf pass on the finished fauna

Measured first. On the standard scenario, fauna on against fauna off (a new
`CF_FAUNA=0` switch, kept for weak machines), interleaved, three runs each:
**135 fps against 148, worst frame 8.1 ms against 6.6**. A tenth of the frame
rate. Inside the fauna slot everything was cheap -- pop tick 0.1 ms, assign
0.02, flock ticks 0.13 -- and the cost was the DRAW: 0.4-0.8 ms to re-stamp 23
animals into a buffer and ~10 MB re-uploaded every frame, for 260,000 vertices
whose only change since the last frame was six floats each of position, yaw and
size.

So the templates are uploaded ONCE, at startup, and each animal is one draw
call from that static slot with its transform as uniforms: the vertex shader
gains a model mode -- place at u_mpos, turn by u_mrot, scale by u_mscale, lit
by u_msky/u_mblk, the normal turned with the body, which also retires the
quarter-turn face hack the stamp needed. The interpolation between ticks is
the only per-frame arithmetic left on the March side.

| | before | after |
|---|---|---|
| fauna draw, per frame | 0.6-1.0 ms | 0.04-0.10 ms |
| fauna on / off, frame rate | 135 / 148 fps | 149 / 150 fps |
| slow frames on the fauna slot, of 600 | 26 | 10 |
| frame budget gate | 8.2 ms | 6.1 ms |

The demo modes still stamp and upload, into a slot of their own so the static
blob is never clobbered; they are inspection tools and the stamp path is what
their tests cover.

What is left on the fauna slot is the population tick's chunk profile, 0.1 ms
ordinarily and 1.4 on a chunk that is mostly water, once every ten frames.
Below the vegetation and water ticks now, and not worth a second pass.

The single-specimen inspector (`CF_FAUNA_DEMO=100+n`, `200+n`) now draws
through the model path too, so the check that a body looks right is a check of
the shader's transform and turned normal, not of the stamp the game no longer
uses for animals. That mattered: a draw count of 36 animals is not evidence
that a new shader path drew anything, and the first in-world screenshot after
the change caught none. The inspector did.
## Points of interest, phase 1: buttes and the arch (2026-09-06)

Design `docs/superpowers/specs/2026-09-06-points-of-interest-design.md`, plan
`docs/superpowers/plans/2026-09-06-poi-phase1.md`.

**The design changed while the plan was being written, and that was the whole
value of writing it.** The first draft gave every point of interest a hashed
*site*, resolved once per lake tile into a `Poi.Field` threaded beside
`Lakes.Tile`. Resolving a site means probing the terrain at its centre, and for
a landform that probe runs inside `Noise.height` -- called for every column of
every chunk heightmap, every apron column, every column of a 128x128 lake pour,
and once per tree and worm placement. It would have had to call `height`
recursively or pay a second full noise stack per candidate cell per column, and
the field that hid the cost needed threading through twenty call sites, a seam
apron, and its own agreement test. None of that was written.

What landed instead is two mechanisms, each already in the codebase:

- **Landforms are masked fields**, the way dunes and glaciers are: a rarity mask
  times a shape, evaluated per column inside `height`, probing nothing. Nothing
  threaded, no seam, `height` still a pure function of the column.
- **Structures are resolved sites**, `Trees.tree_at_cell` exactly, stamped once
  per chunk. Their suitability *is* free to probe `height`, because height
  carries no structure term. `CubeForge.Poi.stamp` is one call at the end of
  `Chunk.generate_lakes` and no signature anywhere changed.

**The regression gate.** `CF_POI=0` zeroes every mask and density, and the
pinned scenario's frame-30 mesh hash is still **380281180** -- the same number
this file has been quoting since the greedy mesher. Worst frame 6.50 ms best of
3 against a 16 ms budget (6.42 before). 473 tests (463 before).

**Cost.** `height` gains one `value2` -- but only after the dune mask is already
up, which is a few percent of columns, and `CF_POI` is not read at all until
then. Best of six, whole-world scalar heightmap: seed 7, 38 ms off / 39 ms on;
seed 15, which has buttes in the window, 37 ms off / 35 ms on. Not measurable.

**Buttes.** The mesa's rise is an **absolute top level** hashed per cell, not a
height added to the ground: a constant rise gives a top that tilts with the
slope under it, and no butte does that. A cell whose ground is already within 8
of its top holds nothing, which is why buttes read as remnants of a former
surface. The radial step is hard, not eased -- an eased rim left a half-height
shelf instead of a cliff -- and the blend that keeps the world from snapping is
the *density* thinning with the country mask, not the mesa shrinking.

Tuning, measured over a 1024-block square at seed 4242 (samples at 8 blocks):

| | desert | country | both | on a mesa |
|---|---|---|---|---|
| threshold 0.70, density scaled by cty x dmask | 411 | 3186 | 151 | 5 |
| threshold 0.62, density `clamp01(2 cty dmask)` | 411 | 4701 | 194 | 13 |

The first is one or two mesas per million blocks -- a lump, not butte country.
The second is 6.7% coverage inside desert-and-country, which is a field. Tallest
mesa in that square: 20 blocks. Buttes turn up in the spawn window of 4 seeds in
40.

**The arch, and two things that were wrong.**

*The span was hashed.* A hashed half-span lands in a real gorge about once in a
thousand cells: **no arch in 160 cells across 40 seeds.** The span is now read
off the terrain -- walk out from the channel until the ground stands
`arch_bank_rise()` above it, both ways, and give up past `arch_max_span()`. An
arch is the span of a gorge, so the gorge should say how wide it is.

*One candidate column per cell.* A channel is a couple of percent of columns, so
a single hashed draw in a 96-block cell finds one about once in forty cells, and
the bank test then throws most of those away. Sixteen hashed candidates per cell
took it to **6 arches in 160 cells** -- roughly one per 230,000 blocks, one seed
in eight with an arch in its spawn window. The extra cost is sixteen `river_at`
evaluations per arch cell and at most four such cells per chunk, about 3% of a
chunk's noise work, and only for cells that pass the density draw.

*A hole at the crown.* The first arch generated had a one-block gap in it. The
underside is an ellipse and an ellipse's slope runs away near the feet -- several
blocks per column, more than the band is thick -- so a band of constant thickness
does not overlap its own neighbours. Each column's band is now taken against its
two neighbours' undersides as well as its own. Found by printing a vertical
cross-section through the arch, which is also how the fix was confirmed:

```
71 .......##..###..          71 ........#...#...
70 ......##########          70 .......#########
69 ....############    <--   69 .....###########
68 ....############          68 .....#######.###   <- the gap
67 ....#####.......          67 .....###........
```

**What is not verified.** Neither shape was confirmed visually. Four rounds of
yaw sweeps at seeds with a POI in the window landed on a peak, in a lake and
under water; the spawn column is chosen by the generator and there is no knob to
put the camera somewhere. The evidence that they are real and right is the
cross-section above, the `poi:` diagnostic line under `CF_TERRAIN_STATS`, the
world and mesh hashes moving at a seed with an arch (seed 1: world 79424082 ->
590784810) and not moving at one without, and the tests. A spawn-position knob
would pay for itself the next time a rare feature needs looking at.

## Points of interest, phase 2: the crystal cavern and the giant tree (2026-09-06)

Two kinds added to the template. Both are **landmarks with no height term**: a
cavern is a hole in the rock and a tree is not terrain, so neither belongs in
`Noise.height`, and `raises(k)` keeps them out of the per-column scan entirely --
the hot path stays exactly two kinds wide however many kinds exist.

**The template gained a volume op**, orthogonal to scatter/landmark: `vol_carve`
(an opening, a chamber), `vol_stamp` (a trunk, a canopy) or `vol_none`. One
function, `vol_block`, decides what a kind does to a voxel, and **the generator
and the preview both call it** -- a preview that modelled the shape separately
would drift from what gets built, which is the class of bug this module exists
to end. Two new rules: a stamp must not also raise the ground, and an
underground kind must keep a roof's worth of rock over its widest chamber
whatever depth it draws (`plo(depth) >= phi(wide) + roof()`).

**Three bugs, each caught by a different one of those two.**

*The preview drew an empty sky.* It asked whether an instance happened to be
placed at the sample cell, and mostly one was not. Shape and placement are now
separated -- `shape_at` is the shape, `rise_at` is the shape once placement
agrees -- which is a better split anyway.

*Every cavern was unlit.* The crystals lining a chamber's shell were being
written under the stamp rule, "only into air", and the voxels they line are rock
that was just carved. The preview showed them; the world had none. The rule is
now split by op: **a carve owns the volume it opens**, so what it puts back
replaces the ground it took; a stamp owns nothing and writes only into air, so a
tree can never eat the hill it stands on.

*The giant tree was a pole.* 34-50 tall with blobs around it -- a conifer, not a
live oak. The references are **wider than they are tall**: a short thick trunk
that boughs leave low down, and boughs that sweep out and *down* before lifting
to their tips. So the trunk is 14-22 with a flaring foot, the boughs are two
segments with a dip at the elbow (that dip is what makes a bough read as
carrying its own weight rather than as a spoke), and the canopy is a flattened
dome of radius `0.92 * reach` sitting on them. Final proportions: about 40 tall
and 70 across.

**The crystals are micro-voxel models, not cubes.** One block per crystal read as
a stack of boxes; Naica's selenite is a shaft. `Model` gained four crystal
templates -- a blade walked from a floor point to a tip, tapering, plus a shorter
one across it -- and `crystal` joined `is_cutout`, so it meshes in the foliage
pass, lets light through and costs no new machinery. The template variant is
hashed on a **coarse** grid so neighbouring blades lean the same way and a patch
reads as one growth rather than a bristle. `Model.count()` 20 -> 24, texture
layers 92 -> 93 (layer 92 is the crystal cube face, still used for the item).

**Numbers.** 478 tests. `CF_POI=0` and `CF_POI=1` agree at seed 7 (mesh
484802240, world 1012862119). Worst frame 6.04 ms best of 5 against a 16 ms
budget, unmoved. Landmarks are rare enough to be landmarks: over 40 seeds, 4
worlds had an arch in the spawn window, 8 a cavern, 3 a giant tree.

`CF_TERRAIN_STATS` now prints a line per landmark with its site, size and
bearing -- and, for a cavern, the world position of chamber 0, because the site
column of a cavern is usually solid rock and standing there shows you nothing.
`docs/poi-arch.png`, `docs/poi-giant-tree.png`, `docs/poi-crystal-cavern.png`.

**Then the beams, the same day.** A model blade cannot be longer than the cell
it is drawn in, so the long crystals could never come from the model layer at
all -- they had to be geometry. A **selenite beam** is a segment through a
chamber, the same primitive the giant tree's boughs already use, made of a
second block (`crystal_beam`, id 70, texture layer 93) that greedy-meshes into a
prism. Two per chamber, radius 1.3-2.5 so a shaft is three to five blocks
across, and half-length 0.95-1.4 of the chamber's radius so each one drives into
the rock at both ends rather than stopping neatly at the wall. Their offsets
from the chamber centre are hashed independently, so they cross rather than all
passing through one point.

Three rounds of tuning, each against the preview rather than a screenshot:

| | beams | radius | half-length | result |
|---|---|---|---|---|
| first | 3 x 3 | 1.6 - 3.2 | 0.95-1.4 r | two of them filled the chamber they were meant to cross |
| second | 3 x 2 | 1.3 - 2.5 | 0.95-1.4 r | still read as pale boulders, not shafts |
| third | 3 x 5 | 0.7 - 1.8 | mixed | shafts |

What fixed it was not the beams but the **chamber**: radius went 8-13 to 12-18
and depth 22-34 to 28-42, because the eye reads a crystal's thickness against
its own length and against the room, and at radius 2.5 in a ten-block chamber
nothing can look like a ten-metre crystal. The beams then went **slender**
(0.7-1.8, so two to four blocks across against a length of twenty to fifty) and
gained a **taper** -- `beam_taper`, full over the middle and drawn to a point at
both ends, the way a crystal terminates -- which needed `seg_t` beside `seg_d2`
so a shape can know where along a segment it is.

Then a **size distribution**, and then the numbers Naica actually has: **seven
beams per chamber, four in five of them large** (radius 1.4-2.4 against a length
of twenty-five to sixty), the rest short slender stubs broken across them. A
chamber where the big shafts are the minority reads as a cave that happens to
have crystal in it; the stubs are what keep the rest from reading as a lattice.

**The angles were the last thing wrong, and the most obvious in hindsight.** The
elevation band was `hash * 0.9 - 0.45` -- plus or minus twenty-six degrees --
so every beam in every chamber lay at much the same attitude and the whole thing
read as a stack of shelves. It is now `hash * 2.4 - 1.2`, plus or minus seventy,
and they cross at genuinely odd angles.

Raising the beam count to seven collided the hash salts: the direction draws
take `110 + 4 * (7j + b)`, which now runs to 193 and ran straight over the
centre offsets at 140 and 161. A salt collision is invisible -- two draws that
should be independent quietly agree -- so the ranges are now laid out with gaps
and the layout is written down where they are declared.

The fine blade growth dropped from 0.26 to 0.18 of shell voxels along the way,
because the beams now carry the look.

Cost: startup meshing 190 ms at a seed with a cavern against 179 with none
(+6%), and the frame budget unmoved at 6.04 ms best of five. Twenty-one segment
tests per voxel sounds like a lot and is not, because the whole beam pass is
behind the `d < 1.45` chamber test.

So the crystals are both things, and each is the mechanism that fits it: the
fine growth is micro-voxel blades, because a cube read as a box; the shafts are
geometry, because a model cannot leave its cell.


## The crystals, actually looked at (2026-09-06)

Four rounds of tuning the beams' length, thickness, count and angle each made
them a bit better and none of them made them look like Naica. The two things
that were wrong were not numbers.

**They were cylinders.** `beam_d` measured a radial distance from the axis, and
a voxelised cylinder is a staircase — round in principle and lumpy in fact. A
selenite beam is a **prism**: big flat planes meeting at hard edges, and that is
most of what makes one read as a crystal rather than as a pale rock. `prism_d`
measures an **octagonal** cross-section instead (four faces read as a crate,
eight as a crystal). The basis is free: for an axis `d`, `e1 = (dz, 0, -dx)` is
perpendicular by inspection and the second component follows from Pythagoras
against the full perpendicular offset, so there is no cross product and no
second basis vector.

**They were the wrong colour, and the texture was not why.** Block light is one
channel and `GLOW` is warm, so every emitter in the game glows the same orange —
a near-white selenite beam rendered beige, and repainting the texture whiter
just made it a paler beige. What fixed it is **effect 12, self-lit** (task 6 of
the phase 1 plan, built early because this is what needed it): an emissive
block's own faces take their glow colour from their own texture,
`glow = (fe == 12) ? t.rgb * 1.15 : GLOW`. Selenite now reads white against
brown rock.

The flag reaches the shim without a second FFI array by riding the **layer
table**: `Texture.selflit_bias()` adds 1000 to a self-lit face's layer and
`cf_vert` strips it and sets the effect. Only `layer_table` carries the bias, so
every March path reads `layer_for` and is unaffected. The water bob had to grow
an upper bound — it fires on `fe >= 2`, which effect 12 would have joined, and
crystals would have wobbled.

The limitation is real and worth stating: this fixes the emitter's **own faces**.
The light it casts on the rock around it is still warm, and making that cold
needs three block-light fields rather than one.

478 tests. `CF_POI=0` and `CF_POI=1` still agree at seed 7 (mesh 484802240) --
the bias touches no block that world contains. Worst frame 7.18 ms against 16.


## Room to be big (2026-09-06)

The shafts still did not read as big shafts, and again the reason was not the
shafts. **Five of them in a chamber barely wider than they were long merged into
one white mass**, and a crystal you cannot see the ends of is not a crystal, it
is a wall. What was needed was air: the chamber went from radius 8-13 to 12-20
and depth 22-34 to 30-44, the beam centres spread to 1.5x the chamber radius so
they stop passing through one point, the count came back to five, and the fine
blade growth halved again to 0.09 of shell voxels.

Two bugs surfaced doing it, both in placement rather than shape.

**`cham_r` could exceed `p_wide`.** It scaled the radius by 0.70 to 1.25, and
the roof rule was written against `phi(p_wide)` — a number the chamber could
therefore beat by a quarter. At the largest radius the caverns broke the surface
and the sea poured in. The multiplier is now 0.62 to 1.00, so `phi(p_wide)` is a
true maximum and the rule means what it says.

**`here` checked the ground at the site only.** A cavern spreads its chambers
thirty blocks sideways, and the site's own column says nothing about the
hillside they run into: chambers came out of the side of hills and flooded.
`cover` now takes the *lowest* ground over a cross of five probes at `p_len`,
and `ground` hangs the chambers off that rather than off the site, so a chamber
under a slope follows the low side. That is the same shape of fix as the arch's
foot probes, and it is the second time this session that a landmark's rules were
right at its centre and wrong over its footprint.

The threshold does far more work than it looks once it has to hold over a
fifty-block cross: at sea + 26 no world in forty seeds had a cavern at all, and
it settled at sea + 18 with the density raised to 0.65.

478 tests. `CF_POI=0` and `CF_POI=1` agree at seed 7 (mesh 484802240). Worst
frame 6.48 ms.

## Points of interest, phase 3a: the canyon and the atoll (2026-09-07)

**A third class: the field.** A canyon is not a thing placed in a valley, it is
what the valley becomes where canyon country is -- so it has no cells and no
site. `cls_field` kinds are evaluated at every column from what `height`
already has in hand (the river mask, its raw crease, the ruggedness, now passed
into `height_at`), gated by a country mask, and they skip every cell rule. The
per-column cost stays where it was: the canyon's octave sits behind the same
cheap gates as the cut itself (above sea + 20, in a valley, in rugged country),
so open lowland never samples it.

**The canyon.** The river mask sharpened into a slot, dropped by
`canyon_depth` (40 young, 60 ancient), quantised to six-block benches so the
walls step, and never cut below the brook's floor. Confirmed by transect before
any camera found it: at seed 24, z 44..60, a slot 10-17 columns wide and up to
five benches deep with stepped edges (`345555531`). `CF_POI_TRANSECT=<z>` is the
new knob -- one character per column across the world, a digit for benches cut,
`+` for raised -- and it is the right instrument for a slot, which is a thing
you read across, not from a camera on the rim that happens to be behind a hill.
`docs/poi-canyon.png`, looking along it.

Canyon country at a threshold of 0.60 put canyons in 20 of 40 seeds, which is
not a landmark; 0.66 now.

**The atoll, and what it found.** A landmark with a CEILING on its ground
(`max_ground`, `cover_hi`), the first kind to want one. Its ring is raised to an
absolute reef flat and its lagoon cut to an absolute floor (rule 3), with one
gap forced below sea level so the lagoon fills by the sea rule and not by the
lake pour, which a tile seam would cut.

Three things went wrong, in order:

1. *No atoll in 140 seeds.* `CF_POI_GATE=1` prints each placement gate's value
   for the atoll's cell, and it said why at once: the ground at hashed sites
   was 49..105 over thirty seeds, the shallowest barely wet. This terrain has
   shelf, not ocean, and "eight under the sea across the ring" never held. The
   lagoon is cut to an absolute floor anyway, so the atoll now only has to
   START under water: `max_ground` sea - 2, probed at half the ring's radius.
   Two of 140 seeds have one.
2. *Placed, but drawing nothing.* `shape_at` at the site said -12 and
   `height_at` said 0. `kind_go` took `max` against a best of zero, which is
   right for mesas and throws away every cut; the atoll is the first kind whose
   shape goes negative. `pick` now lets a nonzero beat zero and only then takes
   the max -- exact, because an instance never overlaps its own kind. The same
   `max` in `height_at`'s combination had the same bug.
3. *A thirty-block wall at the gap.* The gap only suppressed the ring, so the
   lagoon floor met the shelf in a cliff exactly where the channel should be.
   In the gap the ring band is the channel at moat depth and the inner foot
   ramps floor -> moat.

An **apron** -- a moat cut to sea - 6 easing back to the bed over 18 blocks --
gives the ring water to stand in on a shelf. Even so, in this terrain the atoll
reads as a crater lake ringed by hills about as often as a reef in open sea
(`docs/poi-atoll.png`): the surrounding shelf is land a few blocks further out.
That is a property of the world's oceans, not of the kind, and it is left as is
rather than tuned again.

**A shell trap that cost an hour.** Four screenshots from four spawn points came
out identical, twice. `set -- $pos` does not word-split in zsh, so every run got
`CF_SPAWN_X="98 84 270"`, which parses as -1 and falls back to the default
spawn. `${=pos}`.

483 tests. `CF_POI=0` is bit-identical (mesh 484802240). Seed 7 now has a
canyon, so the pinned scenario's mesh with POIs on moves -- to 839500821 at
the 0.60 threshold and **796267894** at 0.66, which is the baseline now. Worst
frame 6.69 ms.

## Points of interest, phase 3b: the delta, and a test suite that lied (2026-09-07)

**The delta** is the second field. Where great-river country meets the coast:
inland, the **trunk** -- the raw crease read against a far lower edge than the
ordinary river uses, so the channel is 20-40 across instead of 3, with its bed
cut below the sea so it holds water its whole length; at the mouth, the
**fan** -- the lobe flattened to a block above the sea and a braid of channels
cut across it. The sediment pass already puts mud and clay under a floodplain,
so the land between reads as delta without new rules. The honest limit stands:
with no flow routing, the river is great only where its country is.

**The crease is a band, not a line.** The river's raw crease is a ridged field,
`1 - |2n - 1|`, so it is above 0.6 wherever the noise is within 0.2 of a half.
The ordinary river's edge of 0.86 slices a 0.07-wide band off that; the delta's
first edge of 0.55 took a band three times wider than intended. Measured by
transect: forty-five columns across at 0.55, up to seventy-eight at 0.60,
twenty to forty at 0.72. Read *across* a river a transect gives its width;
read *along* one it gives its length, which is how a sixty-six-column band at
seed 13 turned out to be a channel and not a bug.

**The fan flattened whole coasts.** Gated on the crease, the fan fired on every
coastal column in great-river country: 11,353 of a window's 16,384 columns
moved at seed 13, fifty-eight columns of seabed raised into land in one row. A
fan is the mouth of *its own* channel and nothing else, so it is now gated on
the trunk's width. Great-river country went from 0.62 to 0.74 as well: nine
seeds in forty have a delta in the spawn window, and one of those has both a
delta and a canyon.

**A mask should gate a field, not scale it.** Both fields multiplied their cut
by the raw country value, so at the country's fringe -- which is most of it,
since a smooth mask is mostly fringe -- the channel was half-dug: the delta's
trunk at seed 13 read as one bench along its whole length, a dry trough with
its bed still above the sea, because the mask there was 0.4. `strength` now
takes the mask to full over most of the country and eases only at the edge,
and the same trunk reads two benches with its bed under water. The canyon had
the same defect and the same fix; seed 7's canyon went back to full depth, so
the pinned scenario's POI-on mesh is **839500821** after all.

**The suite passed with twenty-one tests missing.** `poi_test.march` gained a
binding named `on` -- a keyword, like `by` -- and failed to parse. `forge test`
printed the parse error among four hundred lines of refinement hints, dropped
the module, ran the other 465 of 486 and said `0 failures`. It was caught
because the total on the `Finished:` line went down. GAPS G85; until the runner
fails on a parse error, that number is the check and it must not fall.

486 tests. `CF_POI=0` bit-identical (484802240); the pinned scenario with POIs
on is 839500821 (seed 7 has a canyon and no delta). Worst frame 6.20 ms. Phase 3 is complete: all seven kinds from the original request
are in.

## Open ocean (2026-09-07)

The atoll had nowhere to be. The sea was wherever the land happened to dip
under 62 -- a shelf a few blocks deep -- and "eight under the sea across the
ring" never held; the kind was right and the world was wrong. So the world
gained an **ocean regime**: one very broad octave (`ocean_mask`, 1/250 blocks,
over 0.62) marks ocean country, and there `ocean_of` pulls the land down to an
abyssal floor at sea - 22 with its own gentle relief. It is terrain, so it
lives in `Noise` and not in `Poi`: the lake pour, the beaches, the biome's
distance-to-water and the atoll's placement all read it for free. It is in
`height_x4` too, or the SIMD lanes test would have said so.

**Islands.** The first version pulled everything in ocean country to the floor,
snow-line peaks included; a glacier test at seed 11 said so. The pull is now
full up to sea + 12 and fades over the next 26, so a coast drowns and a
mountain becomes an island. A first attempt at that fade started at the sea
itself, and a coast at sea + 8 kept most of its height -- every atoll site went
dry (seed 54's ground went from 41 to 70).

**The atoll's country is the ocean's.** Giving it an octave of its own meant the
two rarely coincided: a ring probed thirty blocks out from a deep site kept
finding the coast of the ocean's own patch. One in 140 seeds, then five once
`country(k_atoll)` became `ocean_mask` and the ring was allowed to want real
water again (`max_ground` sea - 6, probes at three quarters of the radius).
`docs/poi-atoll.png` is seed 116 from above: a sand-and-grass ring round a blue
lagoon in open sea, with the gap on the bearing.

**Three pinned tests broke, and none of them was a bug in what changed.** Each
was a hunt that asked a simpler question than the generator does, and open
ocean was the first terrain where the answers differed:

- *the biome band scan* -- `Biome.height_of` reads a column's surface with
  water counted as ground (its climate wants that); `World.surface_y` stops at
  the first collidable block, the bed. They agree on land, and every band
  column had been land. The assertion now allows the sea's surface under water.
- *the dune column is sand through* -- gravel on top: the generator gives talus
  precedence over dune sand and reads slope off the PILED heightmap, which a
  hunt over `Noise.slope` cannot see. The hunt now wants a dead-flat column.
- *the bog* -- twice. First the same piled-slope trap; then, with that fixed,
  every bog-shaped flat at seed 11 was the bed of a poured lake, because open
  ocean makes new closed basins at sea level whose rims pour. The hunt now asks
  the lake table too, and tries seeds from 11 up until one has bog.

The pattern: **a hunt must ask the generator's own questions**, or the first
terrain that separates them will fail the test for a reason that is not a bug.

486 tests. Seed 7 now sits partly in ocean country and has an atoll at (94, 11),
so both pinned hashes move deliberately: `CF_POI=0` **1010970938**, POIs on
**866428893**. The `CF_POI=0` world is no longer "the world before POIs" -- the
terrain itself changed -- but it is still the world with every point of
interest off, which is what the gate is for.

## Merged with main (2026-09-07)

Main brought the fauna (a ten-float vertex layout with block light as its own
attribute, and a palette of its own at 23..85), the survey, and a tinted atlas.
Six files conflicted and every one resolved the same way -- both sides kept:
`cf_vert` splits the shade on the CPU as main does *and* strips the self-lit
bias; the atlas is main's tinted one at 94 layers with the two crystal layers
still in it; the crystal's palette entries moved above the fauna's range to 86
and 87. GAPS renumbered: the silent-test-drop finding is G85.

529 tests. Terrain unchanged by the merge -- seed 7's `world` hash is the same
before and after -- but the vertex layout is not, so the pinned mesh hashes
move once more: `CF_POI=0` **360460515**, POIs on **1064367677**. Worst frame
6.93 ms.

## The crystals, revisited with a slice (2026-09-07)

`CF_POI_SLICE=1` prints a plan of the first cavern's chamber 0 at its centre
height and a vertical section through it, drawn through `Poi.vol_block` -- the
same function the generator uses. The preview draws a hypothetical cell on
flat ground; this draws the chamber the camera is standing in, and it said in
one screen what six rounds of cameras had not: **the beams had fused into a
slab** 28 columns wide. Six shafts at radius 2.0-3.3 in an eleven-radius
chamber, each a third of the room, centred near its middle with free hashed
directions, lay together and merged.

Three changes, each against the slice:

- **Thin.** Radius 1.0-1.6: a tenth of the room's radius, a metre-thick crystal
  in a ten-metre chamber, which is Naica's proportion.
- **Fanned.** Azimuths one slot each round the compass with a hashed jitter,
  the way the giant tree throws its boughs, so two shafts never lie parallel
  through the same middle. Elevation +-45 rather than +-70: Naica's lean, they
  do not stand.
- **No popcorn.** The blade models on every surface read as white lumps and
  stole the scene from the shafts; Naica has no fine growth like that. They are
  floor rubble now, at 3% of shell voxels below the chamber's centre.

After: separable shafts, one long diagonal about four blocks thick crossing the
chamber end to end with a second across it, in both the plan and the section.
`docs/poi-crystal-cavern.png` is from the chamber's south side at eye level.

One non-finding worth recording: five floor-level cameras produced frames
pixel-identical to the previous build's, save the frame counter, across a
change the slice showed plainly. The world hash at that spawn responds to
`CF_POI`, so the beams were there; at floor level looking down, a chamber whose
ceiling is white shafts looks the same whether the shafts are a slab or six.
The camera was the wrong instrument; the slice was the right one.

## Coloured block light, in the nibble the byte had spare (2026-09-07)

Every emitter lit the world the same warm orange, so a near-white selenite
beam cast an orange glow on the rock round it, and the self-lit effect (which
fixes an emitter's own faces) could not fix what it cast. The design note said
three fields -- 12 MB instead of 4, three sweeps, a wider vertex. It was not
needed. A block-light byte holds a level of 0..15 in its low nibble and nothing
in its high one, so the high nibble now carries the emitter's **colour index**,
swept along with the level: `give1`, `give_level`, `at_level`, `level_go`, the
gather and the two write sites (`list_go`, `full_row_ip`) compare on the low
nibble and carry `16 * colour` of the source into every neighbour they light.
Seeding writes `pack_bl(emission, emission_colour)`. Skylight's colour nibble is
always zero, so every skylight byte is what it was, bit for bit -- and so is
every block-light byte in a world without a cold emitter: seed 7's pinned
hashes did not move.

From the field to the pixel: `Light.get` strips the nibble, so every caller that
wanted a level still gets one; `Light.colour` reads it. `key_for` reads the
colour at the voxel just outside a face and puts it in **bits 56 and 57** of the
greedy key, above the species, so faces lit by different emitters do not merge;
`cf_quad_into` decodes it and `cf_vert` writes it into bits 16 and up of the fx
word (`Vertex.light_colour_of`); the shader picks `GLOW_COLD` -- (0.82, 0.94,
1.00) -- for colour 1 and the old warm `GLOW` otherwise. The effect id is eight
bits now, not sixteen: the one test that pinned the sixteen-bit ceiling was
pinning an accident of the old layout, and now pins the eight-bit one and that
the colour above it does not leak.

The one behaviour deliberately left: two emitters of different colours within
reach of one voxel do not blend. The voxel takes the colour of whichever gave
it its level -- a hard seam, not a gradient. No world has both kinds in one
cavern today; when one does, the seam is a real limit of one index against
three channels, and it will show.

534 tests. Worst frame 7.90 ms against 16; the drained mesh equals the rebuild.
`docs/poi-crystal-cavern.png` is the seed 18 chamber under its own light.

## The crystals, built from smaller voxels (2026-09-07)

"Those crystals look awful. They should resemble the crystal caves in Mexico."
Then, after two more rounds: "Those are just blocks." They were. A beam one or
two blocks thick drawn as cubes IS a staircase of cubes, and no radius, count,
angle, texture or light had changed that in seven rounds of tuning. The user's
original instruction -- "make the crystals out of smaller voxels" -- was the
answer, and the blade models had been a half-measure: a fixed 8-cube template
per block cannot follow a beam that crosses blocks at an angle.

**What was built.** A beam block is no longer a cube. The mesher cuts each
one into 8x8x8 sub-cubes against the exact prisms of the beams that pass
through it, and greedy-meshes the cut like a mushroom template:

- `Poi.beam_table(seed, wx0, wz0, lx, lz)`: every cavern beam that can reach
  a chunk, as segments (both ends, radius) in the mesher's window-local frame.
  `Poi.cell_aux` is the same table in the generator's frame, built once per
  instance per chunk and threaded through `vol_block` -- the generator no
  longer re-derives thirty beams from the hashes at every voxel.
- `Poi.beam_near` (the beams within a block's half-diagonal), `Poi.beam_grid`
  (the cut, with a one-sub-cube MARGIN from the neighbouring blocks),
  `Model.template_inner` (greedy over the inner cube only; the margin's cells
  cull faces and emit none), `F32Buf.stamp_xf` at scale 10/8 about the
  block's centre to place it. Without the margin a section of beams carried
  1.65 million floats and a quarter of them were faces buried inside the beam
  between two of its own blocks; with it, 1.25 million.
- `crystal_beam` is a cutout: see-through to its neighbours' faces, a model to
  the mesher (no cube faces), opacity 6 to light. `is_model` says so.
- The shape: `beam_taper` is a straight prism over the middle 72% and a
  straight line to a point over the rest -- a chisel end. The quartic taper
  had made every beam a tusk, and a curved outline is the one thing a crystal
  never has. Sixteen beams a chamber (the user: "more of them"), centred
  anywhere in 0.9 of the chamber's radius rather than within 0.5, where ten of
  them had all passed through one hub and the room read as a starburst.
- The light: a self-lit face is shaded by direction (top 1.0, sides 0.80 /
  0.68, bottom 0.52) and by a Fresnel-ish rim, and its glow is a flat 1.10
  rather than the texel squared -- squared, cream came out grey, and unshaded,
  a chamber of beams was a wall of white with no edges. `GLOW_COLD` is a warm
  white-gold now (1.00, 0.97, 0.90): the blue read as an ice cave. No blade
  models anywhere in a cavern; Naica is the beams and the rock.

**Three findings on the way.**

*The generator and the mesher disagreed by half a block.* The generator
samples a block at its integer corner (`u_of` takes wx, wz, y); the mesher
samples sub-cubes outward from the block's min corner. Same beams, two
frames: 148 of one chunk's 1008 beam blocks held no beam by the mesher's
reckoning and were see-through holes. The half is folded into `beam_table`'s
shift once, and `poi_test` now holds the two frames to each other: every
beam block holds a beam, no air block is inside one.

*A discarded array result is a lost write.* `NativeArray.set_f32` returns the
array to use from then on; the first table builder wrote through `let _ =`
and every beam came back as zeros. Both fill passes thread the array now.

*The blue hole that was not a hole.* A sky-coloured wedge below the chamber
floor survived every fix, a queue-lag frame 200 test, and a transect, until a
level shot from the chamber's middle showed no hole at all: the camera had
been standing inside the wall, seeing out through back faces. The chamber is
narrower at y 36 than at y 42.

**Cost, and where it went.** Timed in a release test on the seed 18 cavern
chunk at (240, 112):

| | before | after |
|---|---|---|
| generate the cavern chunk (plain chunk: 2 ms) | 137 ms | **16 ms** |
| cut one section of beams (`beam_geometry`, 1.66 M floats) | 391 ms | **143 ms** |
| remesh that section (re-light from the cache) | 424 ms | **4 ms** |
| worst frame standing in the chamber, 400 frames | 986 ms | **9.6 ms** |

The 986 ms frames were the mesh drain re-cutting beam sections after the
light flood: four sections a frame at ~250 ms each. The cut depends only on
the blocks and the beams, so `ChunkMesh` now keeps it per section
(`beam_sections`, keyed by `Mesher.beam_key`, a hash of which blocks are
beams), stamped UNLIT with each vertex's block index in its light slot, and
every remesh re-lights it through `cf_f32_relight` -- one pass over the
vertices looking the light up per block. The generator's 137 ms was
`beam_min_sd`: 864 thousand calls at 320 ns each in March. The distance and
the grid cut are shim kernels now (`cf_beam_min_sd`, `cf_beam_grid`), and the
March versions stay as `beam_min_sd_ref` / `beam_grid_ref`, held to the
kernels by a test at 4000 points and cell for cell on a real block.

What is still paid: the first cut of a cavern chunk's sections, in the
workers at startup and on a window shift -- 143 ms a section, three or four
sections a cavern chunk. Not in a frame yet; a shift into a cavern is the
next thing to measure.

**Held.** `CF_POI=0` is bit-identical to the previous commit, checked by
building 19a21b2 in a scratch worktree and running both recipes: frames 30 /
dump 20 gives mesh **31924079**, frames 400 / dump 390 (the frame budget
script's) gives **33247928**, before and after. (The values pinned at the
merge were taken with a recipe this entry did not record; these two are the
recipes from now on.) POIs on at frames 400 / 390: **629446512**, unchanged by
everything here, so seed 7's window holds no cavern in view. Frame budget 7.48
ms; the drained mesh equals the rebuild. 538 tests. `docs/poi-crystal-cavern.png`
is four floor-level views of the seed 18 chamber.

The `World` carries its seed now (`World.seed`; `from_chunks` takes it from
the save header), because the mesher needs the beams and the beams come from
the seed.
