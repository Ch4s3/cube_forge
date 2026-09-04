# Weather — overcast, fog, precipitation and a weather actor — design

The day/night cycle gives the world a moving sun, a moon, and a sky colour that
warms into dusk, but the weather never changes: every day is cloudless. This
adds three layers on top of that cycle — an **atmosphere** that can go overcast
and foggy, **precipitation** that falls and is stopped by the terrain above it,
and a **weather actor** that decides when the sky turns and how hard.

Scope is those three layers and the vertex-format widening they need. Weather
that *changes the world* — snow settling as blocks, rain feeding the water
actors, wet surfaces — is deliberately out of scope and recorded in
`todos.md`; see §8 for why it is blocked on biomes rather than on effort.

## 1. Vertex layout: 8 -> 9 floats

Precipitation needs per-vertex alpha, and the current layout (pos.xyz, uv,
layer, shade, face) has no spare field. Lighting already took this step once,
7 -> 8 for `a_face`, so the path is known.

The ninth float is **not** plain alpha. It is a packed effect word:

```
packed = float(effect * 256 + round(alpha * 255))
```

Floats represent integers exactly up to 2^24, so this is lossless: 16 usable
bits of effect (0..65535) and 8 bits of alpha, decoded in GLSL with integer ops rather
than a fractional-part trick that would collide at `alpha = 1.0`:

```glsl
int   q = int(a_fx + 0.5);
int   e = q >> 8;
float a = float(q & 255) / 255.0;
```

Effects:

| value | meaning |
|-------|---------|
| `0`   | none — world geometry, HUD, outline, marker |
| `1`   | precipitation — unlit, alpha-blended, not fogged |
| `2+`  | reserved (wet sheen, wind sway) |

The reserved range is the point of packing rather than spending the float on
alpha alone: the next effect that wants a per-vertex flag costs no memory and
no attribute.

**Every existing vertex writer pushes the single constant `255.0`** (effect 0,
alpha 1). Touched: `mesher.march`, `marker.march`, `outline.march`,
`hud.march`, the `/ 8` vertex-count divisions in `cube_forge.march`, and the
two `glVertexAttribPointer` blocks in `cf_shim.c` (attribute 5 at byte offset
32, stride 36). Mechanical, but wide — it is the riskiest part of the change
precisely because it is boring, so it lands first and alone (§7).

Cost: +12.5% vertex memory, roughly 1 MB at a full 244k-vertex world.

## 2. Atmosphere

`gfx_begin_frame(r, g, b)` already computes the sky colour on the March side,
swinging blue toward a warm horizon and scaling by the sun's intensity ramp.
The shim **stashes that same colour as the fog colour**. Two consequences, both
free: fog can never disagree with the sky, and distant geometry dissolves into
the horizon instead of popping at the far plane. No second colour path exists
to drift out of sync.

Two new uniforms:

- `u_fog_density` — exponential fog, `f = 1 - exp(-d * density)`, applied as
  `mix(color, fogColor, f)` in the fragment shader. Exponential rather than
  linear so it does not depend on the far plane. Clear weather is a very low
  density (haze, not nothing); storm is roughly an order of magnitude higher.
- `u_overcast` — 0..1, how much cloud sits between the sun and the ground.

Overcast is a **separate uniform and is not folded into `u_sun`**. That value
doubles as the sun's *height*: it drives the moon ramp, the warm-to-white
colour blend and the dusk amount. Dimming it to fake cloud would silently move
sunset and summon the moon at noon. Instead `u_overcast` does what cloud
physically does — scales the direct term down, lifts ambient, and softens
shadows:

```
shad = mix(shad, 1.0, u_overcast)
```

Under full cloud, shadows vanish and the light goes flat and diffuse, which is
the correct look and also *cheaper* than a clear day, since the trace early-outs
in the shadow code fire more often.

### Rejected

- **A skybox or cloud layer.** Real clouds want a second geometry pass, a
  texture and a scrolling UV; the sky here is a clear colour, and keeping it
  that way is what makes the fog trick free. Overcast reads as overcast through
  colour, fog and flat light alone.
- **Volumetric or god-ray fog.** Per-fragment ray-marching for scattering, on
  top of the shadow trace already in the fragment shader. Not worth it.

## 3. Precipitation: a stateful pool

A new module, `lib/cube_forge/precip.march`. A fixed-size pool in a
`NativeFloatArr`, four floats per particle (x, y, z, vy). `CF_PRECIP` sets the pool's
**capacity** (default 4000); weather intensity decides how much of it is live,
so clear weather costs nothing and a storm costs the cap. Each frame: integrate, apply the wind vector, and
respawn any particle that has left a cylinder around the eye at a fresh point
on that cylinder's top face.

A **pool, not a procedural lattice.** A stateless hash-per-cell scheme is
cheaper and trivially deterministic, but it can only ever draw falling dots: a
particle with no state cannot collide, splash, pool, or gust around an
obstacle. The pool is chosen for what it makes possible later, and §8's
follow-on work depends on it existing.

### Stopping at the terrain

**A particle dies when it enters a cell whose baked skylight is zero.** One
lookup into the light field per particle per frame.

This is one line and it is exactly right. Rain stops under an overhang, never
appears inside a cave, and respects player edits the moment they are relit —
none of which a `Noise.height` query gives you, since that ignores every block
the player has placed or broken.

### Geometry

Rebuilt each frame into an `F32Buf` and uploaded to free slot **249** (250 is
the marker, 251 icons, 252 the hotbar, 253 the underwater tint, 254 the
outline). Rain draws as a velocity-aligned streak, snow as a camera-facing
square; both carry effect `1`, so the fragment shader skips lighting and fog
for them. Precipitation is deliberately **not** fogged: it is the thing
generating the fog, and mixing it toward the fog colour would make a storm's
rain fade out exactly as the storm peaked. Distance falloff comes from its own
alpha instead. Drawn through `cf_gfx_draw_translucent` with depth writes off, after
the water pass.

Budget: 4000 particles x 6 verts x 9 floats is about 864 KB rebuilt and
uploaded per frame — the same order as the world's own upload, and the reason
`CF_PRECIP` exists. Measured and recorded in `RESULTS.md`.

### Determinism

The pool breaks the property that a given frame index renders identically,
which `CF_DUMP_FRAME` relies on. Bought back in two moves: the respawn LCG is
seeded from the world seed, and when `CF_MAX_FRAMES` is set the integration
uses a fixed `1/60` dt instead of the wall clock. Headless dumps stay
reproducible byte-for-byte.

## 4. The weather actor

One `WeatherActor`, spawned at startup beside the water actors, following the
shape `water.march` already established: a `WTickReq`-style call handler and
`Actor.call(pid, ..., 2000)` from the frame loop.

It is **ticked on the existing `tick_period()`, once per ten frames**, not once
per frame. The frame loop caches the returned value and eases toward it in
between, so the actor costs one message per ten frames and the visible weather
still changes smoothly.

State is a coarse phase — clear, cloudy, rain, storm — with a dwell time drawn
per transition, plus a continuous 0..1 intensity that lerps toward the phase's
target so nothing ever snaps. Intensity drives `u_fog_density`, `u_overcast`
and the precipitation particle count together, which is what keeps the three
layers reading as one weather system rather than three effects that happen to
be on at once.

`CF_WEATHER` pins the intensity, exactly as `CF_SUN` pins the sun angle, so a
storm can be inspected without waiting for one.

### Lightning

During storms, strikes fire on a Poisson-ish schedule and set a new `u_bolt`
uniform: a ~120 ms spike on ambient with a double-flicker envelope. It is a
distinct uniform from `u_flash`, which is the player's flashlight.

### Rain or snow

Keyed on `Noise.snow_line()` (95), the constant that already decides where the
terrain wears snow, with a blend band beneath it where both fall.

There are no biomes to ask (§8), and inventing a second altitude constant would
let the weather contradict the ground it falls on. Reusing `snow_line` means
snow falls exactly where snow already lies.

## 5. Module boundaries

- `precip.march` — the pool and its geometry. Knows the light field and the
  camera; knows nothing about weather phases. Takes an intensity and a
  rain/snow mix as arguments.
- `weather.march` — the actor and the phase machine. Pure state and
  transitions; touches no buffers and no GL.
- `cube_forge.march` — asks the actor on the tick boundary, eases the cached
  value, and passes the result to `precip` and to the two new uniforms.

The split is what keeps the phase machine unit-testable without a window.

## 6. Verification

- **Packing round-trip.** `pack(effect, alpha)` then decode over the full
  effect range and all 256 alpha steps; asserts the integer-exactness claim in
  §1 rather than trusting it.
- **Phase machine.** Driven with a fixed seed for a simulated day: asserts every
  phase is reachable, dwell times stay in range, and intensity is continuous —
  no step larger than the per-tick ease.
- **Particle bounds.** After N updates with a fixed seed, every live particle is
  inside the cylinder and above the terrain; none sits in a zero-skylight cell.
- **Pixel test.** Build a sealed roof, pin `CF_WEATHER` to storm, dump the
  frame, and assert no precipitation pixels appear beneath the roof while they
  do appear beside it. This is the skylight rule under test end to end.
- **`CF_WEATHER=0` renders identically to today** apart from the vertex stride —
  the regression that catches an accidental fog or overcast term leaking into
  clear weather.

## 7. Landing order

1. **Vertex 8 -> 9 alone**, every writer pushing `255.0`, shader decoding but
   ignoring the effect. Nothing looks different; a frame dump before and after
   should match. Landing the wide, boring change by itself means a later
   rendering bug is never ambiguous about its cause.
2. **Atmosphere** — fog and overcast, driven by `CF_WEATHER`, no actor yet.
3. **Precipitation** — the pool, still on `CF_WEATHER`.
4. **The actor** — phases, transitions, lightning.

## 8. Out of scope: weather that changes the world

Snow settling as blocks, rain topping up the water actors, wet surfaces
darkening, splashes where particles die. Recorded in `todos.md`.

The blocker is **biomes, not effort**. The terrain work that shipped under
`2026-09-03-terrain-biomes-design.md` is altitude banding — `sea_level` at 62,
`snow_line` at 95, granite above a slope threshold — not biome regions. There
is no query that answers "what kind of place is this?", so there is nothing to
key an accumulation rate, a precipitation type, or a drying rate off beyond
height. Global weather over an altitude-banded world is honest; *accumulating*
weather over one is not, because it would put the same snow depth on every
mountain in the world on the same schedule.

Biomes first means a region map — temperature and humidity fbm, sampled per
column — and that is its own design.

The other reason to defer: accumulation is not really a weather problem. It is
world mutation on a timer, and it lands on the chunk remesh path, the per-chunk
dirty flags and the `WaterChunk` protocol simultaneously. Rain feeding water in
particular needs a rule for unbounded sources, or ponds grow without limit
under the current finite-level flow.

## 9. Risks

- **The vertex widening is wide and dull.** Mitigated by landing it alone (§7)
  with a frame dump proving nothing changed.
- **Fill rate.** Thousands of alpha-blended quads is the classic voxel-game
  frame killer, and depth-write-off means no early-Z. `CF_PRECIP` bounds it and
  `RESULTS.md` records it.
- **Determinism.** The pool is the first stateful renderer-side simulation;
  §3's fixed dt and seeded LCG are what keep headless dumps meaningful, and the
  `CF_WEATHER=0` regression test in §6 is what proves it.
- **Overcast interacts with shadows.** Softening via `mix(shad, 1.0, overcast)`
  is a cheat, not scattering. It looks right and costs nothing; it will not
  survive close inspection of a half-clouded sky, which is out of scope.
