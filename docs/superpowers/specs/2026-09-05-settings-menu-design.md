# Settings menu — graphics options, applied live and remembered

## Problem

Every tunable is a `CF_*` environment variable read once at startup. A player
who wants fullscreen, no vsync, shorter shadows or less rain has to relaunch
from a shell with the right variables set, and nothing remembers the choice.
The menu (`lib/cube_forge/menu.march`) has a main page and a slots page and no
way to change anything about how the game renders.

## What we are building

- **A SETTINGS item** on the main page of both the start screen and the escape
  menu, opening a settings page.
- **Five graphics rows**, each with a left and a right arrow button:
  fullscreen, vsync, shadow distance, soft shadows, precipitation.
- **Changes apply the moment an arrow is clicked.** No OK, no Apply, no
  restart.
- **Choices are remembered** in a text file in the save directory, read before
  the window opens so fullscreen and vsync hold from the first frame.

Out of scope for this cut: game options (day length, mouse sensitivity, audio),
debug overlays, keyboard navigation of the rows, per-save-slot settings.

## Constraints discovered before designing

- **The HUD font has digits and A-Z only** (`Hud.letter`). Arrow buttons need
  `<` and `>` masks added to the same 3x5 table.
- **Shadows are already live.** `Win.gfx_set_shadow` is called every frame
  from values in the Knobs record (`lib/cube_forge.march:1923`), so the
  shadow rows need no new plumbing beyond reading a different source.
- **Precipitation is a pool sized once** (`Win.precip_init`) and a per-frame
  live count clamped to that pool. A lower setting is a lower clamp, not a
  smaller pool.
- **Vsync and fullscreen are decided inside the shim** by `getenv` in
  `cf_win_open` (`native/cf_shim.c:131-140`). They become shim calls that March
  makes, so the same path serves startup and a live change.
- **The save header format** (`Save`, `key value` lines, unknown keys ignored)
  is the house style for a text file and is reused, with one deliberate
  difference noted in §4.

## Design

### 1. `CubeForge.Settings` — the value

Pure data, no I/O. Five fields, each stored as an index into a fixed list of
choices so stepping is one operation for every row.

| Field | Choices (display) | Game value | Default |
|---|---|---|---|
| `fullscreen` | OFF, ON | 0, 1 | OFF |
| `vsync` | OFF, ON | 0, 1 | ON |
| `shadow_dist` | OFF, 16, 32, 64, 128 | 0, 16, 32, 64, 128 blocks | 64 |
| `soft_shadows` | OFF, ON | 0, 1 | ON |
| `precip` | OFF, LOW, MEDIUM, HIGH | 0, 1000, 2500, 4000 particles | HIGH |

```march
type Settings = Settings(Int, Int, Int, Int, Int)   -- one index per field, in table order

fn fields() : Int                                  -- 5
fn field_fullscreen() : Int ... fn field_precip() : Int   -- 0..4
fn name(f : Int) : String                          -- "FULLSCREEN", "VSYNC", "SHADOWS", "SOFT SHADOWS", "PRECIPITATION"
fn choices(f : Int) : Int                          -- how many
fn choice_label(f : Int, i : Int) : String         -- "OFF", "64", "MEDIUM" ...
fn choice_value(f : Int, i : Int) : Int            -- the game-facing number
fn index(s : Settings, f : Int) : Int
fn value(s : Settings, f : Int) : Int              -- choice_value(f, index(s, f))
fn label(s : Settings, f : Int) : String           -- choice_label(f, index(s, f))
fn step(s : Settings, f : Int, dir : Int) : Settings   -- dir is +1 or -1, wraps
fn defaults() : Settings
fn nearest(f : Int, v : Int) : Int                 -- the choice index whose value is closest to v
fn with_value(s : Settings, f : Int, v : Int) : Settings   -- set by game value, via nearest
```

Defaults equal today's `CF_SHADOW_DIST` 64, `CF_SHADOW_SOFT` 1, `CF_PRECIP`
4000 and the shim's vsync-on, fullscreen-off, so a player with no settings
file sees no change.

**Environment overrides.** `CF_FULLSCREEN`, `CF_VSYNC`, `CF_SHADOW_DIST`,
`CF_SHADOW_SOFT` and `CF_PRECIP` keep their meaning for scripted runs. If set,
the variable's value replaces the loaded setting through `with_value`. An
override is never written back to the file. This lives in `main`, where the
other knobs are read, not in `Settings`.

### 2. The settings page

`Menu` gains a third page, `page_settings`, and a `Settings` field:

```march
type Menu = Menu(Bool, Int, Int, Int, Int, Int, Int, Int, List(SlotInfo), Settings)
fn settings(m : Menu) : Settings
fn set_settings(m : Menu, s : Settings) : Menu
```

**Main page.** Four items: NEW GAME, LOAD/SAVE GAME, SETTINGS, QUIT.
`item_settings` is 2 and `item_quit` becomes 3. The main panel's bottom edge
and `seed_y0` move down one row pitch (0.18) so the seed field stays below the
last item. Nothing else on the main page changes.

**Settings page.** Five rows and BACK, laid out like the slots page: same
panel width (0.80), same row pitch (0.14), same glyph size (`slot_px`). Each
row is three boxes:

```
 [<]  SHADOWS                    64  [>]
```

- The row body carries `Settings.name(f)` left-aligned and
  `Settings.label(s, f)` right-aligned, with 0.014 of padding.
- The arrow boxes are squares of side `item_h` at either end of the row, with a
  0.01 gap to the body, drawn with the new `<` and `>` glyphs centred.
- BACK is a plain full-width row under the five, as on the slots page.

**Item indices on this page.** `item_at` returns one integer as everywhere
else; on the settings page it encodes row and part:

```
item = row * 3 + part      part: 0 = left arrow, 1 = body, 2 = right arrow
item_back() on this page = fields() * 3   (15)
```

Helpers `settings_row(item)`, `settings_part(item)`, and constants
`part_left`, `part_body`, `part_right` keep the encoding in one place. Hover
brightens whichever box the cursor is over, as now, so the focused index is
this same encoded item.

**Geometry accessors.** Every box comes from `Menu`, as today:
`item_x0(m, i)` gains an item argument (the main and slots pages ignore it),
`item_w(m, i)` likewise; `item_y0(m, i)` uses the row. The HUD and the hit test
read the same functions, so drawn boxes and clickable boxes cannot drift.

**Behaviour.** Clicking a left or right arrow calls `Settings.step` on that
row's field and stores the result in the menu. Clicking the body does nothing.
Esc goes back to the main page, like the slots page. There is no keyboard
navigation of the rows.

**The click.** `Menu.click(m, item) : Menu` is a pure step: it returns the menu
after a click on `item`, which on the settings page means the stepped
settings, and on other pages the menu unchanged. The frame loop calls it and
then handles the side effects (§3, §4) by comparing before and after.

### 3. Applying

Nothing restarts. The frame loop reads `Menu.settings(menu)` once per frame
into a local `s` and:

- **Shadows.** `Win.gfx_set_shadow(if map_mode do 0.0 else value(s, shadow_dist) end, value(s, soft_shadows) == 1)`
  replaces the Knobs-sourced call. `shadow_dist` and `soft_shadows` leave the
  Knobs record.
- **Precipitation.** The pool is initialised once at
  `max(choice_value(precip, HIGH), CF_PRECIP)` so an override can still ask for
  more than the menu offers. The per-frame clamp `live <= precip_cap` becomes
  `live <= value(s, precip)`. `precip_cap` leaves the Knobs record.
- **Vsync and fullscreen** are edge-triggered. After the click step, the loop
  compares the old and new settings and calls, for each field that changed:

```march
Win.set_vsync(on : Bool)         -- cf_win_set_vsync: glfwSwapInterval(on ? 1 : 0)
Win.set_fullscreen(on : Bool)    -- cf_win_set_fullscreen
```

  Both are also called once at startup, right after `Win.open`, from the
  loaded settings. `cf_win_open` loses its two `getenv` checks.

**`cf_win_set_fullscreen` in the shim.** Going in: record the window's
position and size in four statics, then `glfwSetWindowMonitor` onto the
primary monitor at its current video mode, exactly the startup path today.
Coming out: `glfwSetWindowMonitor(g_win, NULL, x, y, w, h, 0)` with the
recorded rectangle. If the window is already in the requested state the call
is a no-op. The existing framebuffer-size callback handles viewport and
projection; nothing else in March needs to know.

Headless runs (`CF_HEADLESS=1`) skip both window calls, as they skip
`Win.open`. As built, `CF_PRECIP` stays a raw particle cap rather than going
through `nearest`: scripted runs use values the choice list does not offer,
and the pool is sized to the larger of the cap and the largest choice.

### 4. Persistence

One file, `settings.txt`, in `Save.dir()` (`CF_SAVE_DIR` or `saves`). Same
shape as the save header, one `key value` per line, values are the game-facing
numbers:

```
version 1
fullscreen 0
vsync 1
shadow_dist 64
soft_shadows 1
precip 4000
```

Game values rather than indices so the file stays readable and a later change
to a choice list does not reinterpret old files. Parsing maps each number back
through `Settings.nearest`.

**Lenient on purpose.** The save header refuses to load when a key is missing,
so a truncated header cannot half-load a world. A settings file is different:
the worst outcome of a missing or bad key is a default value, so missing keys
default, unknown keys are ignored, unparseable numbers default, and a missing
or unreadable file yields `Settings.defaults()`. `version` is written for the
future and not checked.

```march
-- in CubeForge.Settings (this is its only I/O; it needs IO.FileRead / IO.FileWrite)
fn encode(s : Settings) : String
fn parse(text : String) : Settings
fn path(dir : String) : String        -- dir ++ "/settings.txt"
fn load(dir : String) : Settings      -- defaults on any failure
fn store(dir : String, s : Settings) : Bool
```

`load` runs once in `main` before `Win.open`. `store` runs after every arrow
click that changed a field. On a failed write the row shows FAILED for
`Menu.status_frames()` (60 frames), through the existing status slot and kind,
with the row index in place of the slot number; the in-memory setting still
applies. An environment override is never written back: `main` keeps the
settings as loaded from disk alongside the set of overridden fields, and every
write takes an overridden field's value from the on-disk copy rather than from
the live settings. A scripted run therefore never clobbers the player's file
with its own values, while a field the player changes in that run is still
saved.

### 5. Testing

- **`test/settings_test.march`**: defaults match the table; `step` wraps at
  both ends on every field; `nearest` picks the closest choice for exact,
  between, below-range and above-range numbers; `encode` then `parse`
  round-trips every choice of every field; `parse` of an empty string, of a
  file with a missing key, an unknown key, and a non-numeric value each yield
  the default for the affected fields and the given values elsewhere.
- **`test/menu_test.march`**: the main page has four items and `seed_y0` is
  below item 3; on the settings page the three boxes of every row are
  disjoint from each other and from the neighbouring rows; each box is hit at
  its centre and `settings_row`/`settings_part` recover the row and part;
  BACK is hit; `click` on a right arrow steps that field by +1 and on a left
  arrow by -1, and on a body leaves the menu unchanged; Esc from the settings
  page lands on main.
- **`test/font_test.march`**: `<` and `>` are defined, non-zero, distinct from
  each other and from every digit and letter.
- **Scripted end-to-end** (windowed with `CF_FRAMES`, since headless mode
  returns before the frame loop): a `CF_AUTOSETTINGS=<frame>` knob opens the settings
  page at that frame and clicks the shadow row's right arrow five frames
  later, the way `CF_AUTOMENU` scripts the restart. The run asserts by reading
  `settings.txt` back: `shadow_dist 128`. Run with a scratch `CF_SAVE_DIR`.
