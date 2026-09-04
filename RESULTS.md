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
