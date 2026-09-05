# Save and load — a start screen, five slots, and an escape-menu save

## Problem

A world lives exactly as long as the process. The seed is printed so the
terrain can be regenerated, but everything done to it (blocks mined and placed,
water that has flowed, the inventory) is gone at quit. There is also no start
screen: the game generates a world from the seed and drops the player into it.

## What we are building

- **A start screen** at launch: NEW GAME with an editable seed, LOAD GAME, and
  QUIT, drawn over the freshly generated world.
- **Five save slots**, listed identically on the start screen's LOAD page and
  on the escape menu's SAVE page. Saving into a used slot overwrites it.
- **SAVE GAME** in the escape menu, opening the slot page. The escape menu does
  not load; loading is a start-screen concern.
- **A save restores** the world's blocks (water as it stands), the player's
  position, velocity and look direction, the inventory and selected slot, the
  seed, the time of day, and the weather phase. Water in motion settles again
  after load. Skylight, occupancy, meshes, the biome field and the spring flags
  are rebuilt, not saved.

## Constraints discovered before designing

- **Actor messages cannot carry native arrays** (GAPS.md G44). Each water actor
  regenerates its own chunk from the seed on `WLoad`. A saved chunk therefore
  cannot be handed to an actor; the actor reads it from disk itself (§4). When
  March lets messages carry byte arrays, the frame loop will send the chunk it
  already read and that message goes away. This is a stopgap by design.
- **The HUD font is digits and upper-case letters only**, so every label is
  chosen from those.
- **A chunk is already a flat 65536-byte array**, and `Bytes.to_u8_arr` /
  `Bytes.from_u8_arr` bridge it to `File.read` / `File.write`. No encoding step
  is needed for the bulk of a save.
- **World setup is `run_session`**, which takes a seed and returns the seed of
  the next session. The start screen and slot loads both fit that loop.
- **The stdlib has no compression.** A slot is 4 MiB raw. Accepted.

## Design

### 1. `CubeForge.Menu`: modes and pages

The menu becomes a small state machine. Two modes, two pages:

| mode   | main page                         | slots page              |
|--------|-----------------------------------|-------------------------|
| start  | NEW GAME, LOAD GAME, QUIT, seed   | SLOT 1..5, BACK (load)  |
| escape | NEW GAME, SAVE GAME, QUIT, seed   | SLOT 1..5, BACK (save)  |

```march
type Menu = Menu(Bool, Int, Int, Int, Int, Int, Int, Int)
-- open?, seed under edit, focused item, mode, page, status slot, status kind, status frame
```

- **Start mode** opens at frame zero of the first session only, with the title
  CUBE FORGE above the panel. NEW GAME with the seed unchanged closes the menu
  and play begins in the world already behind it. A changed seed restarts the
  session exactly as the escape menu does today.
- **Slots page.** Five rows and BACK. A used row reads `SLOT 1 SEED 12345`
  with the save date and time (`2026 09 05 14 30`) on a second line. An empty
  row reads `SLOT 1 EMPTY`. A click on a row saves into it (escape mode) or
  loads it (start mode). A click on an empty row in start mode does nothing.
  Esc or BACK returns to the main page; Esc on the main page closes the menu in
  escape mode and does nothing in start mode.
- **Status.** A row shows SAVED for one second after a save, or FAILED after a
  failed save or load; the menu stays open on failure. Status is (slot, kind)
  plus the frame it was set, cleared by the frame loop after 60 frames.
- **Geometry.** Panel height and item spacing are functions of the page. Both
  drawing (`Hud.build_menu`) and `item_at` read the same accessors, the rule
  the escape menu already follows.
- **Slot labels** come from a listing built once when the slots page opens
  (§2), held in the menu as a `List(SlotInfo)`, not re-read every frame.

### 2. `CubeForge.Save`: format and files

A slot is a directory:

```
<save_dir>/slot<N>/
  header.txt
  chunk_00.bin ... chunk_63.bin
```

- `save_dir` is `saves` under the working directory, or `CF_SAVE_DIR` when
  set. Created with `Dir.mkdir_p` on first save.
- `chunk_NN.bin` is the chunk's 65536 bytes as they are, `NN = cx + 8 cz`,
  two digits zero-padded.
- `header.txt` is one `key value...` pair per line, space separated:

  | key         | value                                                    |
  |-------------|----------------------------------------------------------|
  | `version`   | `1`                                                      |
  | `side`      | world side in chunks (`8`)                               |
  | `seed`      | the session seed                                         |
  | `saved_at`  | unix seconds, for the slot label                         |
  | `player`    | px py pz vx vy vz yaw pitch                              |
  | `inventory` | 72 ints (item, count per slot) then the selected slot    |
  | `day`       | seconds into the day cycle                               |
  | `weather`   | phase, ticks left, intensity, bolt                       |

  Floats are written with `float_to_string` and parsed with the stdlib float
  parser. Unknown keys are ignored; missing keys are an error.

- **Write order: header last.** A directory with chunks but no header is
  incomplete, lists as EMPTY, and cannot be loaded. A crash mid-save never
  leaves a loadable half-world. Overwriting writes in place; no temp directory.
- **Validation** (`Save.check(dir, slot) : Result(Header, String)`): the header
  parses, `version` and `side` match the build, and all 64 chunk files exist
  with exactly 65536 bytes. Anything else is `Err(reason)`.
- **Listing** (`Save.list(dir) : List(SlotInfo)`) reads only each slot's
  header. `SlotInfo(slot, seed, saved_at)`; a slot whose header is missing or
  unparsable is reported empty.

Public surface: `encode_header`, `parse_header`, `write(dir, slot, world,
header)`, `read_chunks(dir, slot, side) : Result(Array.PVec(Chunk), String)`,
`check`, `list`, `chunk_path(dir, slot, i)`. The module needs `IO.FileRead`
and `IO.FileWrite`, and `IO.Process` for `CF_SAVE_DIR`.

### 3. Session flow (`lib/cube_forge.march`)

- **Start descriptor.** `run_session` takes `Start = FromSeed(Int) |
  FromSlot(Int)` instead of a bare seed, and the frame loop's "next session"
  field holds a `Start` (with a none case) instead of a seed. NEW GAME and a
  slot pick both go through the same restart path.
- **Loading a world.** For `FromSlot`, read the header, then build the chunk
  array from the 64 files with a new `World.from_chunks(chunks, side)`. From
  there the pipeline is the new-game one unchanged: skylight flood, occupancy,
  meshing, upload, biome build, spring flags. Player and inventory come from
  the header instead of the spawn search.
- **Day time.** The session carries a day offset in seconds. The sun angle is
  computed from `(clock - session_start + offset)`; a new game starts at 0, a
  load at the header's `day`. Saving writes the current value.
- **Weather.** Load sends `send_seed` then `send_restore(phase, ticks,
  intensity, bolt)`, in that order, so the fresh phase never overwrites the
  saved one.
- **Saving.** On the slot click the frame loop calls the weather snapshot,
  then `Save.write` synchronously. A few milliseconds for 4 MiB, accepted as a
  one-frame hitch on an explicit action.
- **Start screen.** The first session opens the menu in start mode at frame 0
  unless `CF_AUTOSTART=1` or the run is headless. Later sessions start closed.
- **Knobs.** `CF_AUTOSTART=1` skips the start screen. `CF_AUTOSAVE=<frame>`
  with `CF_AUTOSAVE_SLOT=<n>` saves at that frame; `CF_AUTOLOAD=<frame>` with
  `CF_AUTOLOAD_SLOT=<n>` loads at that frame, once, in the style of
  `CF_AUTOMENU`. All existing scripted commands keep working by adding
  `CF_AUTOSTART=1`; headless runs need nothing.

### 4. Water actors (`lib/cube_forge/water.march`)

A new message:

```march
on WLoadSlot(cx : Int, cz : Int, n : Int, seed : Int, dir : String, slot : Int)
```

The actor reads its own chunk file and, for its four edge mirrors, the edge
columns of its neighbours' chunk files (four extra 64 KiB reads per actor).
Then it rebuilds the spring list and marks every water cell dirty, as
`load_sim` does after generating. The module gains `needs IO.FileRead`.

If a read fails after the frame loop's validation passed (the files vanished
in between), the actor generates from the seed instead and prints the chunk
index. The rendered world and the actor then disagree for that chunk. Logged,
not hidden; a race that requires deleting files mid-load is acceptable.

### 5. Weather actor (`lib/cube_forge/weather.march`)

Two new messages: a snapshot call replying with the phase's four fields
(packed as a `List` of the two ints and two floats scaled to ints by 1e6, since
call replies follow the same no-native-array rule and a small list is
simplest), and `WRestore(phase, ticks, intensity, bolt)`.

### 6. Error handling

- **Save:** stop at the first failed write, print the reason, show FAILED.
  Chunk files already written stay; without a header the slot is EMPTY.
- **Load:** `Save.check` runs before anything is torn down, while the current
  session still runs. Only a valid slot triggers the restart, so a bad slot
  never leaves you without a world. FAILED on the row, reason on stdout.
- **Version:** a mismatch is a failure, not a migration. No compatibility
  promise until a save survives a released build.
- **Paths:** slots are bounded 1..5 by the menu; `CF_SAVE_DIR` is trusted like
  every other knob.

## Testing

- **Save unit tests** (`test/save_test.march`): header round trip including
  negative floats and pitch; rejection of a missing key, wrong version, wrong
  side; chunk round trip through a scratch directory; a slot with chunks but no
  header lists as empty; `check` fails on a short chunk file.
- **Menu tests** (extend `test/menu_test.march`): mode and page transitions;
  labels per page; `item_at` on the taller slots page returns each row from its
  drawn centre; Esc on the slots page returns to main; Esc on the start main
  page does not close.
- **World test:** `from_chunks` then writing the chunks back gives identical
  bytes.
- **Water actor test** (extend `test/flow_test.march`): write a chunk with a
  pool to a scratch slot, send `WLoadSlot`, tick, and check the actor's water
  count matches the file and its springs are found.
- **Weather test:** snapshot then restore on a fresh actor reproduces the
  phase fields.
- **End to end** (`scratch/saveload.sh`): run one with `CF_SEED=7
  CF_AUTOSAVE=200 CF_AUTOSAVE_SLOT=1 CF_SAVE_DIR=<scratch>` and a state dump
  at frame 200; run two with `CF_AUTOLOAD=30 CF_AUTOLOAD_SLOT=1` and a dump at
  the frame after the load. The `world` and `biome` hashes must match. Two
  processes, so a script rather than a march test.

## As built, 2026-09-05

- The main page holds three items plus the seed field, so its panel runs to
  -0.56; the slots page is wider (±0.40, rows 0.66 wide) and set in a smaller
  face so `SLOT 5 SEED 1073741823` fits a row. Both pages still hit-test and
  draw from the same accessors, now all functions of the menu (its page).
- `Save.check` validates chunk sizes by reading the files: `File.stat`'s
  record is private to the stdlib. `FileError` is private too, so errors name
  the path and the operation, not the cause.
- `&&` is strict in March (GAPS G33): the slot validation is behind an `if`,
  not an `&&`, or it would run every frame for slot 0.
- A new game keeps the process clock for the sun, as before; only a load sets
  a day offset. The two are one formula: `clock - session start + day0`.
- The weather snapshot is a second call handler; call sentinels route by
  constructor tag, so `WxTickReq | WxSnapReq` is one type in handler order.
- `CF_AUTOSLOTS=<frame>` was added for a headless look at the slots page.
- The menu moved to VBO slot 246: the shim's spray pool had taken 247 and was
  overwriting the menu's buffer every frame.

## Out of scope

Compression, more than five slots, delete, loading from the escape menu,
progress indication during the load (which blocks like a new game does),
and save compatibility across versions.
