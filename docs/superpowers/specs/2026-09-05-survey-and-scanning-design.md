# Survey and scanning — design (2026-09-05)

## The problem

This world simulates more than it shows. Springs feed lakes, lakes set moisture,
moisture and temperature classify the biome, the biome decides the surface and
what grows, mycelium competes on climate fitness and pushes the climate back.
All of that runs, and none of it reaches the player.

The loop we want is already tested. `CF_AUTOCANAL` digs a line of water beside
the player "so the biome field sees it the way a real edit would", and the field
answers: moisture reaches 24 columns (`Biome.reach`, 4 for a small body), the
axes ease at 1/10800 a tick — about eleven ticks a second, so a threshold
crossing lands in three to five minutes — and the banks reclassify, retexture
and grow trees. A player digging that same channel is told nothing, sees a
change they cannot attribute, and has no reason to have dug it.

So this is not a simulation feature. It is the instrument that makes the
simulation playable, and a reason to go looking.

## The loop

Explore to find a species growing wild. Scan it, which is what teaches you the
climate it wants. Survey a place to learn what it is and which way it is
drifting. Reshape it — water, or altitude — until it suits the species you
know. Plant. Watch it take, and push the climate further on its own.

Exploration supplies the goals; terraforming is how you spend them; the
existing fruit-and-effects chain pays for it.

## Three levers, all already simulated

The player can change a place's climate three ways today, and is told about
none of them:

| lever | mechanism | reach |
|---|---|---|
| water | dig a channel or pond; moisture rises with proximity | 24 columns, 4 for a small body |
| altitude | `Biome.lapse` is 0.012 a block, so digging 28 blocks down is +0.33 temperature — a whole band | local |
| mycelium | established species pull the climate toward themselves (`CF_MYC_FEEDBACK`) | the network's reach |

The survey does not add levers. It makes these three legible, and it does that
by showing the INPUT beside the value it drives, so the rule is discovered
rather than announced.

## What the survey is

One instrument, one key (`Q`), two surfaces.

### The panel, in first person

For the column under the reticle:

```
GRASSLAND        SETTLED
TEMP  58    HEIGHT 81
MOIST 24    WATER 31 AWAY
WOULD THRIVE  MEADOWBELL 71
NEEDS WETTER  MARSHLIGHT
```

- Line 1: the biome, and the trend. Trend is exact, not a guess: the field holds
  both the eased axes and can compute the targets, so this is `classify` at
  target compared with `classify` now. `SETTLED` when they agree, `BECOMING
  DESERT` when they do not.
- Lines 2-3: the two axes, each beside the input the player can change.
  `HEIGHT` and `WATER N AWAY` are the terraforming controls, sitting next to
  the numbers they move. Nobody is told that digging down warms a place; they
  dig, watch `HEIGHT` fall and `TEMP` climb, and work it out.
- Lines 4-5: species advice, and ONLY for species that have been scanned.

The HUD font is uppercase letters and digits; punctuation renders blank
(`Hud.glyph_for`). The wording above is written to that constraint rather than
around it.

### The drift overlay, in map view

Map view already draws a biome map. Under survey it also marks the columns that
are CHANGING — which the field already knows, because the dirty set added for
the tick's performance (`Biome.active`) is exactly the set of columns that are
not settled. The optimisation doubles as the visualisation: upload that array
the way `biome_map_upload` uploads the biome ids.

This is the surface that makes a three-minute shift feel like something you
caused. Irrigate a valley and it lights up as changing, then goes quiet as it
settles.

## Scanning, and why the survey is gated

The place half of the survey — biome, axes, height, water distance, trend — is
always available. It is measurement, and a thermometer needs no unlocking.

The species half is earned. Find a species growing in the world, look at it,
press `E`. That species becomes known, and from then on the survey will name it
as a candidate anywhere its band fits, and say which axis is short.

- Before any scan the survey is a thermometer: it tells you what a place is and
  where it is going, and nothing about what to do with it.
- After six scans it is a planning tool: every place you look at is read
  against everything you know.
- A species growing in front of you that you have not scanned reads
  `UNKNOWN SPECIES  SCAN` — the prompt is the hook.

The gate is what makes this an explore game rather than a menu. The six species
(`Species.name`: Frostcap, Pinewart, Meadowbell, Lanterncap, Marshlight,
Sunshelf) live in different climate bands, so knowing all six means having been
somewhere cold, somewhere hot, somewhere wet and somewhere dry. The map is the
tech tree.

Scanning requires the species to be ALIVE in the world — mycelium or fruit, not
a spore in the inventory. Spores are found; habitats must be visited.

## Data

- **Known species**: a bitmask, one bit per species, six bits. It rides in the
  save header, which is `key value` text lines, as one integer. A save without
  the key loads as zero — an old save knows nothing, which is correct and needs
  no migration.
- **Trend**: computed on demand for one column when the panel is open. No
  storage, no per-tick cost.
- **Drift overlay**: the existing `Biome.active` array, uploaded on the same
  cadence as the biome map.

Nothing is added to the per-frame path. The panel builds only when its text
changes, the way the ground readout already does.

## Keys

`Q` toggles the survey; `E` scans. Neither is bound today — the shim wrapper
gains two bindings, as it did for the escape menu's `R`, `Enter` and
`Backspace`.

## Headless verification

Following the house pattern of scripted knobs:

- `CF_AUTOSURVEY=<frame>` toggles the panel on at a frame, so a dump can assert
  its text.
- `CF_AUTOSCAN=<frame>` scans whatever is under the reticle.
- `CF_KNOWN=<mask>` presets the known set, so the panel's gated lines can be
  tested without walking the world.

Checks worth writing:

- The panel's text at a pinned frame and seed is stable.
- `BECOMING X` agrees with what the column actually classifies as once eased:
  run the canal scenario, assert the trend says `BECOMING FOREST`, run on and
  assert the biome becomes forest.
- A scan sets exactly one bit, is idempotent, and survives save and load.
- The gated lines are absent at `CF_KNOWN=0` and present for the scanned
  species only.

## What this does not do

- It adds no new simulation. Every number it shows is already computed.
- It does not tell the player the species bands outright. It says a species
  would thrive, or which axis is short, for species they have scanned. The
  bands themselves stay implicit.
- It does not add terraforming tools. Digging and placing are the verbs; the
  survey is what makes them purposeful. Dedicated channelling or levelling
  tools are a separate question, deliberately left open.

## Decisions

**Scanning reads both fruit and mycelium** (decided 2026-09-06). Fruit is what a
player notices first -- it is the visible, findable thing -- and mycelium is the
species' actual presence, so a patch with no fruit standing on it is still a
specimen. Requiring fruit alone would make discovery depend on the fruit cycle's
timing; requiring mycelium alone would send players hunting for a texture they
have no reason to read yet. Both, and the prompt appears for whichever is under
the reticle.

**The trend ignores the hold counter** (decided 2026-09-06). The question was
whether `BECOMING DESERT` is premature when the hold counter would still
suppress the flip. It is not, because the two answer different questions. The
trend is a DESTINATION: `classify` at the targets, which the axes reach over
minutes at 1/10800 a tick. The hold counter is a DEBOUNCE at arrival: thirty
consecutive ticks of disagreement, which at about eleven ticks a second is under
three seconds. A column reading `GRASSLAND ... BECOMING FOREST` during those
three seconds is telling the exact truth twice over -- it is grassland, and it
is becoming forest. Consulting the counter would only make the instrument
silent about a change that is genuinely underway.

## Open questions

- **Panel legibility.** Five lines of a 3x5 glyph font at menu pixel size may
  crowd. May need a smaller pixel size or fewer lines. An implementation
  detail, to settle when the panel is laid out against a real frame rather than
  in advance.
