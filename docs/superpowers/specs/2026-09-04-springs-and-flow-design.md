# Springs and flowing water — design

Water above sea level now conserves volume, which means nothing above sea level
flows for long: a placed block is a puddle, and a puddle stops. Springs put
sustained flow back, on purpose and in bounded amounts, and the rendering shows
it moving.

Scope: a water item that is finite and a spring item that is not; generated
springs; evaporation, which is what keeps an infinite source from filling a
valley; flow direction carried in the vertex; a time uniform that scrolls,
bobs and foams the water surface; and spray particles at drops and spring
mouths. Landing order in §9. **Spring rate, evaporation and generation density
are knobs, and their defaults are found by measurement during the build (§8),
not chosen here.**

## 1. Two water items

The finite-water work changed the scripted `place_water` path only. Two in-game
paths still make infinite sources: the hotbar's water slot places id 4 verbatim,
and `Inventory.harvest` collects *any* water cell as id 4 — scooping a puddle
yields a spring.

| item | collects | places |
|------|----------|--------|
| **water** (hotbar slot 4) | from any water cell | **seven units**, id 5 |
| **spring** (new) | only by breaking a spring block | id 4 |

The spring gets texture layer 15 — a pale ring on the water blue — so its icon
is not a lake.

## 2. A spring is a trickle

`process` on a source gives `(8 - n) / 2` to each lower neighbour and never
decrements: 8-16 units a tick on a slope, forever. Evaporation cannot balance
that — the equilibrium would be a thousand thin cells. So a source is capped:
**at most `spring_rate` units per tick, total across its neighbours**, default
1, knob `CF_SPRING_RATE`. Below sea level the lake rule (`source_neighbour`)
still promotes neighbours to sources, so the sea is unchanged; the cap only
matters above it, where water is finite.

A spring on a slope is a brook that runs until evaporation matches the trickle;
on flat ground a small puddle that stops growing; into a lake, a river — sources
absorb, so that case is bounded outright. A dammed brook fills to the dam and
overflows, because evaporation (§3) hits thin spreading water, not the full
cells of a rising pool.

**Sealed pits still fill, slowly.** One unit a tick into a sealed pit is one
block every few hundred ticks. That is what water does; a finite aquifer
recharged by rain would bound it and ties into weather — recorded as the
follow-up in `todos.md`, not built here.

## 3. Evaporation

A volume cell at **level 1 with sky above it** — nothing but air all the way to
the top of the world — loses its unit with probability `1 / evap` **once per
tick**, `evap` default 16, knob `CF_EVAP`. Sources never evaporate. Deeper water
never evaporates directly; it thins first.

*As built:* a thin cell that survives its draw is put on a wake list and
re-marked at the start of the next tick, not re-queued immediately — re-queuing
let it be drawn dozens of times within one tick and a puddle dried in one. The
same once-a-tick marking is how a spring gives exactly `spring_rate` a tick:
neighbours never re-mark a spring; `tick` does, once.

Two effects: a brook reaches a length instead of a valley, and stray puddles
dry. The hash is `Noise.hash2` on the cell and the tick, so it is deterministic
for a seed and reproducible under `CF_FRAMES`.

The closed-basin conservation test gains a roof (sky above is what evaporates);
a new open-basin test asserts a puddle *does* shrink to nothing.

## 4. Generated springs

At `Chunk.plant`, beside trees: one draw per 8-block cell with the trees'
`cell_hash` idiom (salt 6). A cell holds a spring when the draw is under
`spring_density` (knob `CF_SPRING_DENSITY`, per mille, default found in §8) and
the cell's canonical column has **slope ≥ 2 and height ≥ 72** — a mountainside,
not a plain, using the height/slope proxies trees use because the biome field
does not exist at generation. The spring block is set at the surface. Positions
are collected so the frame loop knows every spring (§7) without scanning.

Springs placed in-game are added to the same list; broken ones removed.

## 5. Flow direction in the vertex

`water_faces` already sees each cell and its neighbours. It computes:

- **direction**: toward the lowest of the four horizontal neighbours; *falling*
  when the cell below is air or lower water; *still* when nothing is lower.
- **speed**: the level drop to that neighbour, 0..7.

packed into the `fx` word: effects **2-9** the eight compass directions, **10**
still, **11** falling; the alpha byte carries speed. The fragment shader treats
that byte as speed for effects ≥ 2 and forces alpha to 1 — water's real alpha
is in its texture. No new attribute, no per-frame CPU.

## 6. The shader: time, scroll, drift, bob, foam

A `u_time` uniform, set once a frame from `Win.time()`.

- **scroll**: for effects 2-9 the ripple UV moves along the direction at
  `0.05 + speed * 0.04` per second.
- **drift**: effect 10 moves slowly on a fixed diagonal, so lakes are not
  painted.
- **bob**: the vertex shader adds `0.03 * sin(u_time * 1.7 + x * 1.3 + z * 0.9)`
  to y for any water effect. Cosmetic: below what the raycast or collision
  notices.
- **foam**: a new layer 16, lighter and whiter; blended in by `speed / 7` for
  effects 2-9 and fully for 11.

**`CF_TIME` pins `u_time`** to a constant. Without it every animated frame
breaks every pixel comparison the project relies on; it lands with the uniform.

## 7. Spray

The C particle pool gains a second kind — white, ~0.6 s life, up then down —
capped by `CF_SPRAY` (default 512), fed by an emitter list the frame loop hands
it each tick as a packed `NativeIntArr`:

- **drops**: when `give` moves units down into a cell that has air *beneath the
  landing*, the actor appends a **kind-4 reply entry** with the landing cell.
  `apply_reply` gains a branch for it. That is a waterfall's foot.
- **spring mouths**: every spring above sea level bubbles, two particles a
  tick, from the list in §4.
- **placement**: placing a spring emits a one-tick burst of forty.

## 8. Finding the balance

The three knobs interact, and the right defaults are measured, not derived.
The measurement is one number per configuration: **active water cells** — the
dirty-queue length summed over chunks — once a fresh world settles (~600
frames), plus the water tick's cost from its existing verbose print.

| knob | start | what it moves |
|------|-------|---------------|
| `CF_SPRING_RATE` | 1 | brook length; churn per spring |
| `CF_EVAP` | 16 | brook length the other way; how fast puddles dry |
| `CF_SPRING_DENSITY` | 40 (per mille of cells) | springs per world (~10 at 40) |

Targets: a brook of **15-40 cells**, visibly running, that does not visibly
grow after settling; the water tick under **2 ms** with every spring active; a
finite bucket's puddle dry within a couple of minutes. Adjust one knob at a
time against the terrain map with `CF_NOMOUSE=1 CF_TIME=0`. The defaults that
land are the ones measured, recorded in `RESULTS.md` with the numbers.

## 9. Landing order

1. **Water items and the trickle** — hotbar water finite, spring item, rate cap.
   The in-game flood is reachable today; this closes it.
2. **Evaporation** — with the roofed and open basin tests.
3. **Flow in the vertex + the shader** — scroll, drift, bob, foam, `CF_TIME`.
   Visible on lakes and streams at once.
4. **Generated springs** — now that they cannot flood and their brooks animate;
   the §8 balancing happens here.
5. **Spray** — drops, mouths, bursts.

## 10. Verification

- Conservation under a roof; evaporation to zero without one.
- A spring on a test slope: volume stabilises within N ticks and stays within a
  band; the source's per-tick give never exceeds `spring_rate`.
- `fx` direction and speed from a 3x3 of levels, unit-tested per case.
- Kind-4 entries appear exactly at drops of two or more, never elsewhere.
- Frame dumps with `CF_TIME=0` are pixel-identical between runs; with it unset
  they differ, which proves the animation is live.
- The hotbar cannot produce id 4 except from a spring item.

## 11. Risks

- **Perpetual churn** is inherent: evaporation removes, the source refills,
  every tick, for the life of the world. The trickle keeps it to the brook's
  tips; §8 measures it.
- **Sealed pits fill** — slowly, by design; the aquifer follow-up bounds it.
- **Bob moves geometry the game logic does not see.** Kept under 0.05 blocks.
- **Placement bursts and the pool cap**: forty particles from a burst share the
  512 with everything else; a burst may be clipped, never the drops.
