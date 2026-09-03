# cube_forge

A minimal Minecraft-like voxel renderer written in [March](https://github.com/march-language/march)
as a dogfooding exercise. The engine is the vehicle; **`GAPS.md`** is the
output: every stdlib gap, annotation friction and codegen problem it surfaced,
with reproduction probes under `probes/`. Timing numbers are in `RESULTS.md`.

Milestones M1–M5 are complete and were each run before the next was started:
window + triangle, one chunk, camera/physics, 8x8 noise world via two `pmap`
stages, and DDA block selection with break/place.

## Layout

```
native/cf_shim.c          the entire unsafe surface: GLFW window, GL 3.3-core/GLES-3.0 subset, input struct
native/glad/, native/KHR/ generated GL loader (glad2, gl:core=3.3, no extensions), committed
lib/cube_forge/ffi/       two extern blocks = two capability domains (Window, Input), each wrapped
lib/cube_forge/f32buf     growable uniquely-owned f32 buffer (the mesher's output type)
lib/cube_forge/math/      Vec3, Mat4, Quat — stdlib-candidate code
lib/cube_forge/{chunk,world,noise,mesher,player,raycast,outline,hud,texture,water}.march
lib/cube_forge/water.march  finite-spread simulation: pure Sim step + one WaterChunk actor per chunk
lib/cube_forge.march      entry point, frame loop, scripted verification modes
test/math_test.march      forge test suite
probes/                   one small project or file per GAPS.md finding
```

## Build and run (executed on macOS arm64, 2026-09-03)

Prerequisites: the March toolchain (`march`, `forge`), Homebrew GLFW
(`brew install glfw`; 3.5.1 was installed, the code uses only the 3.4 API),
and `z3` on PATH for refinement checking.

The project pins its compiler with `.march-version` (see GAPS.md G27 for why
this matters): the file names a toolchain under `~/.march/versions/`. It is
pinned to a march 0.3.0 build that carries the `runtime/pin-main-thread`
runtime patch (GAPS.md G15: `MARCH_PIN_MAIN=1` keeps `main` on the OS main
thread while the other scheduler workers keep running). To reproduce that
entry from the march repo, check out branch `runtime/pin-main-thread`,
`dune build`, then:

```bash
V=~/.march/versions/$(cat .march-version)
mkdir -p $V/bin $V/runtime $V/stdlib
cp <march>/_build/default/bin/main.exe $V/bin/march
cp <march>/runtime/* $V/runtime/
cp <march>/stdlib/*.march $V/stdlib/
```

Without the patch (stock 0.3.0, e.g. a symlink farm onto `~/.opam/march`) the
engine still runs, but only with `MARCH_NUM_SCHEDULERS=1` (see below).

Then:

```bash
forge check                 # typecheck (+ refinement obligations)
forge build --release       # native binary at .march/build/release/cube_forge
forge test                  # 8 unit tests (math, buffer)
forge lint --strict
```

Run it. The window must be created on the OS main thread; March runs `main`
on whichever scheduler worker picks it up unless told otherwise (GAPS.md
G15), so:

```bash
MARCH_PIN_MAIN=1 ./.march/build/release/cube_forge
```

`MARCH_PIN_MAIN=1` pins the `main` green thread to scheduler 0 (the process
main thread) and leaves the other scheduler threads running, so world
generation and meshing stay parallel (measured: `mesh all` 349 ms vs 436 ms).
On a runtime without the patch the variable is ignored and the shim refuses
to open the window; fall back to the old workaround, which serialises `pmap`:

```bash
MARCH_NUM_SCHEDULERS=1 ./.march/build/release/cube_forge
```

Controls: click to capture the mouse, mouse-look, WASD, Space to jump (or swim
up in water), left click breaks, right click places the selected block, keys
1–4 select grass/dirt/stone/water, Esc quits. The FPS counter is top right.

### Headless / scripted modes (used for every verification in this repo)

| env | effect |
|---|---|
| `CF_HEADLESS=1` | no window: generate + mesh the world, print timings, exit (needs neither `MARCH_PIN_MAIN` nor `MARCH_NUM_SCHEDULERS`) |
| `CF_FRAMES=N` | exit after N frames |
| `CF_DUMP=path.bmp CF_DUMP_FRAME=N` | dump the back buffer at frame N (convert with `sips -s format png`) |
| `CF_AUTOWALK=1` | hold W (collision test) |
| `CF_AUTOSPIN=<mrad/frame>` | scripted mouse-look |
| `CF_AUTOEDIT=<frame>` | break the targeted block at that frame, place stone 60 frames later, water 120 frames later |
| `CF_AUTOSWIM=1` | spawn over a lake, sink, hold Space from frame 320; prints y and in-water state |
| `CF_AUTOFLOW=<frame>` | place a water source at that frame, break it 600 frames later; prints per-chunk water counts and per-tick timings |
| `CF_ALLOC_PROBE=1` | print net live-object deltas for each per-frame piece |
| `CF_WORKERS`, `CF_SEED` | pmap worker count, terrain seed |

Example (what produced `RESULTS.md`):

```bash
CF_HEADLESS=1 ./.march/build/release/cube_forge
MARCH_PIN_MAIN=1 CF_FRAMES=460 CF_AUTOEDIT=330 CF_DUMP_FRAME=420 CF_DUMP=/tmp/m5.bmp ./.march/build/release/cube_forge
```
