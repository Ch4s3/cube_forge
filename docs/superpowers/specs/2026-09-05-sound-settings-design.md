# Sound settings — master, ambience and music behind a settings hub

## Problem

The settings menu (`docs/superpowers/specs/2026-09-05-settings-menu-design.md`)
covers graphics only. The mixer has four ambience beds and one music voice and
no way to turn any of it down short of `CF_AUDIO=0`, which kills the device.

## What we are building

- **Three volume rows**: MASTER, AMBIENCE, MUSIC, each OFF / 25 / 50 / 75 / 100.
- **A settings hub.** SETTINGS opens a page with GRAPHICS, SOUND and BACK; each
  of the first two opens a row page laid out like today's settings page.
- **Volume applied in March**, where the frame loop already sends the mix once
  per tick. No shim change.
- **Remembered** in the same `settings.txt`, under three new keys.

Out of scope: per-bed volumes, sound effects (there are none), an environment
override for volume, keyboard navigation.

## Design

### 1. Model

`Settings` grows from five fields to eight. The new fields follow the graphics
five in table order:

| Field | Index | Choices | Game value | Default |
|---|---|---|---|---|
| `master` | 5 | OFF, 25, 50, 75, 100 | 0, 25, 50, 75, 100 | 100 |
| `ambience` | 6 | same | same | 100 |
| `music` | 7 | same | same | 100 |

`Settings` becomes an eight-int constructor; `fields()` is 8. `name`, `key`,
`choices`, `choice_label`, `choice_value`, `index`, `set_index` extend to the
three, so `step`, `nearest`, `encode`, `parse`, `merge` work unchanged. A
convenience `fraction(s, f) : Float` returns the value over 100.

Defaults of 100 mean an old file with no sound keys, or no file at all, plays
exactly today's mix.

`CF_AUDIO=0` keeps its meaning: no device, regardless of volume.

### 2. Pages

`page_settings` becomes the **hub**: three main-style items, `hub_graphics`
(0), `hub_sound` (1) and BACK (2), on the main page's row width and pitch,
without a seed field. Two new pages, `page_graphics` and `page_sound`, are
**row pages** with the layout the settings page has today: arrows, body, BACK.

A row page is two numbers:

```march
fn page_field0(p : Int) : Int   -- graphics 0, sound 5
fn page_rows(p : Int) : Int     -- graphics 5, sound 3
fn is_row_page(p : Int) : Bool
fn field_of(m : Menu, item : Int) : Int   -- page_field0(page) + settings_row(item)
```

Item encoding, `item_at`, `click`, `label`, `value_label`, `set_status` and the
status row all stay as they are, except that the field a row edits is
`field_of` rather than the row index, and `item_back` on a row page is
`page_rows * 3`. `panel_y0` shrinks for the sound page's three rows so the
panel does not hang empty under them.

**Navigation.** Main SETTINGS goes to the hub. Hub GRAPHICS and SOUND go to
their row pages. BACK on a row page goes to the hub. BACK on the hub goes to
main. Esc follows BACK on every sub-page: row page to hub, hub to main.
`CF_AUTOSETTINGS` opens `page_graphics` directly, so the scripted graphics
check is unchanged.

### 3. Applying

In the frame loop's audio tick, before `Aud.set_mix` and `Aud.note`:

```
amb   = fraction(master) * fraction(ambience)
music = fraction(master) * fraction(music)
set_mix(rain * amb, wind * amb, leaves * amb, water * amb, cutoff, music_level * music)
note(hz, dur, timbre, note_gain * music)
```

The cutoff is untouched: it is a filter, not a level. Everything else in the
audio path, the schedule included, runs as before, so muting the music does not
change which note comes next when it is turned back up.

### 4. Testing

- `settings_test`: `fields()` is 8, the three defaults are 100, `fraction` of
  each choice, the file round-trips the new keys, an old five-key file parses
  with sound at 100.
- `menu_test`: the hub has three items whose centres hit; SETTINGS from main
  lands on the hub; GRAPHICS and SOUND land on row pages with the right row
  count and BACK index; `field_of` on the sound page's first row is `master`;
  a right-arrow click on that row steps master; Esc from a row page reaches the
  hub and from the hub reaches main.
- End to end, with the null audio device (`CF_AUDIO=2`, `CF_AUDIO_DUMP`): a run
  with a settings file saying `master 0` tees a WAV whose samples are all zero;
  the same run at `master 100` has non-zero samples.
