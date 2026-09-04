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

- **G59** — G21 (a borrowed read before a consuming update turns FBIP into a
  full copy) makes a BFS queue over a `NativeU8Arr` impossible: a 1M-iteration
  read-then-write probe reached a 229 GB peak footprint. Propagation is a
  level-synchronous downward sweep instead, where every pass reads one array and
  writes a different one. `cf_u8_blit` and an early-out before the chunk lookup
  took the lighting tests from 49 s to 21 s.
- **G60** — `World.block_at` per voxel (chunk coords re-derived plus a PVec trie
  walk, 727 ns each) was 179 ms of the relight's 217 ms. Hoisting the chunk out
  of the column loop cut seeding 15x and the full flood from 1 724 ms to 521 ms.
