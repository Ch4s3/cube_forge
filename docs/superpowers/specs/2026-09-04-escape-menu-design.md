# Escape menu — quit, and a new game with an editable seed

## Problem

`Esc` quits the program outright (`lib/cube_forge.march:885`). There is no way to
leave without losing the session, and no way to start a different world without
restarting the process with `CF_SEED=<n>` on the command line. The seed is
printed at startup precisely so a world can be reproduced, but nothing in the
running game can act on that.

## What we are building

`Esc` opens a menu over the dimmed world with three things on it:

- **NEW GAME** — regenerate the world from the seed in the field below.
- **SEED** — an editable field, pre-filled with a fresh random seed.
- **QUIT** — close the window, as `Esc` does today.

`Esc` again closes the menu and returns to play.

## Constraints discovered before designing

- **The HUD font is digits only.** `Hud.glyph` holds 3x5 masks for 0-9 and
  nothing else, so no existing code can draw a word.
- **March has no `Actor.stop`.** A session spawns 64 water actors and one
  weather actor. If a new game spawned a fresh set, every restart would leak the
  previous one, and nothing in the language can reclaim them.
- **The actors can be reseeded instead.** `Water.send_load(pid, cx, cz, n, seed)`
  makes a chunk actor regenerate itself from a new seed, and
  `Weather.send_seed(pid, seed)` reseeds weather. Reusing the pool sidesteps the
  missing `stop` entirely, so the actor pool outlives a session by design rather
  than by accident.
- **World setup is one linear block** running from terrain generation to the
  single `frame_loop` call at the end of `main`. Restarting requires extracting
  it.

## Design

### 1. Letter glyphs (`lib/cube_forge/hud.march`)

Extend the existing 3x5 mask table with A-Z and add `emit_text`, which walks a
`String`'s characters and emits one glyph each. Same masks, same renderer, same
vertex layout as the digits — no new machinery.

This lands first because everything else draws through it.

**Test:** all 26 letters are defined, none is zero, and no two are equal. A
duplicated mask is invisible in a screenshot; that is exactly how two hotbar
slots came to render identically during M6.

### 2. `CubeForge.Menu`

A new module holding menu state and geometry, with no I/O:

```march
type Menu = Menu(Int, Int, Bool)   -- focused item, seed under edit, open?
```

Public surface:

- `item_at(ndc_x, ndc_y) : Int` — which item the cursor is over, or -1.
- `append_digit(m, d)`, `backspace(m)`, `reroll(m, entropy)` — seed editing.
- `seed(m) : Int`, `is_open(m) : Bool`, `focused(m) : Int`.
- the drawing geometry (`item_x0`, `item_y0`, …) used by both the renderer and
  `item_at`.

**Drawing and hit-testing must derive from one geometry source**, the rule the
inventory already follows: a layout change that moved the drawn boxes but not
the hit boxes would leave every unit test green and the menu unusable.

The seed is clamped to the 30-bit range the world generator uses
(`int_and(…, 1073741823)`), so a long typed number cannot produce a seed the
generator will not accept.

### 3. Seed editing

The field starts at a fresh random seed, derived the same way `main` derives one
today. Number keys append a digit, Backspace removes one, `R` rerolls, `Enter`
starts the game.

`key_0` (48), `Backspace` (259), `Enter` (257) and `R` (82) are not bound in
`lib/cube_forge/ffi/input.march` today and are added there.

### 4. Session restructuring (`lib/cube_forge.march`)

Extract terrain generation through `frame_loop` into

```march
fn run_session(seed : Int, …) : Int
```

returning the seed of the next session, or -1 to quit. `main` loops on it.

Across a session boundary the window, GL context, uploaded texture, projection
matrices and the actor pool all survive. Only `Scene`, the chunk meshes and the
player are rebuilt. The water actors are reseeded with `send_load` and the
weather actor with `send_seed`, so no actor is created or abandoned.

Quit continues to route through `Win.request_close()`, so the existing exit path
and its end-of-run statistics are unchanged.

### 5. Behaviour while the menu is open

The world dims and player input is suspended — the same treatment the inventory
already gets, and consistent with it. The cursor is released, as the inventory
does.

Starting a new game blocks for the ~1.5-2 s of terrain, skylight flood,
occupancy build and meshing, with no progress indication. Making that
asynchronous is a much larger piece of work and is deliberately out of scope.

## Testing

- **Font:** every letter defined, non-zero, and distinct.
- **Geometry:** `item_at` returns each item when handed that item's own drawn
  centre, and -1 outside the panel.
- **Seed state machine:** append, backspace, clamping at the 30-bit bound,
  reroll changing the value, and `Enter` yielding the field's value.
- **Restart, end to end:** a `CF_AUTOMENU` scripted knob in the style of
  `CF_AUTOINV` drives a headless restart. It asserts that the same seed twice
  produces identical vertex counts and that a different seed produces a
  different world — the property that would actually break if the session
  rebuild missed a piece of state.

## Out of scope

- Asynchronous or progressive world generation.
- Saving or loading worlds.
- Any menu item beyond the three above.
