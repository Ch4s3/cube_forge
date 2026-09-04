# Procedural sound and music — design

Date: 2026-09-04

The world is silent. This adds a generative score and four ambience beds, on the
same March-owns-policy / C-owns-the-inner-loop split that `Precip` already uses.

## Why the split is what it is

March's per-element `NativeArray` writes are O(n) on a shared array, so a
per-sample loop is O(n^2) (GAPS G68, and G54 for a buffer held in a persistent
`Array`). Rendering audio in March means 48,000 of those a second. `Precip` hit
the same wall at 4,000 particles — 5.9 fps for the step alone, the geometry
build SIGKILLed — and resolved it the same way: policy in March, the loop in C.

So: March decides *what* should be heard, six floats and the odd note at a time.
C decides *how to make that sound*, at 48 kHz, on its own thread.

Nothing is loaded from disk. There are no audio assets in this repo and this
design adds none — every bed is filtered noise, every note is an oscillator.

## Modules

| Module | Owns |
|---|---|
| `CubeForge.Tone` | The twelve-tone row: seeded generation, the four transformations, transposition, pitch class to Hz |
| `CubeForge.Wind` | Gust strength and direction as a pure function of `(tick, seed, weather)` |
| `CubeForge.Audio` | Mixer policy: world state to bed gains, master cutoff, and the next-note decision |
| `CubeForge.Ffi.Audio` | Thin extern wrappers; a new `Audio` capability domain |
| `native/cf_audio.c` | The miniaudio device and the 48 kHz callback: one voice, four beds, one filter |

`miniaudio.h` is vendored under `native/` (single header, public-domain/MIT
dual-licensed, no new link dependencies beyond the system frameworks it already
selects at compile time). `forge.toml`'s `[ffi] sources` gains `native/cf_audio.c`.

`Wind` is deterministic and stateless — two octaves of `Noise.hash2` over the
tick counter, smoothed. It gusts, it is seeded, it is the single source of truth
for both the mountain bed and the leaf bed, and it needs nothing threaded
through `Frame`. If precipitation later wants a slant, the same function
answers.

## Data flow

Once per `tick_period()` — every 10 frames, about 6 Hz, the cadence `Weather`
already runs at:

```
frame_loop
  |- gather: biome at player, eased temperature, weather intensity,
  |          Precip.snow_mix, Player.eye_in_water, altitude exposure,
  |          nearby-leaf fraction
  |- Wind.at(tick, seed, wx)                      -- pure
  |- Audio.mix(that lot) -> Mix                   -- pure, six floats
  |- Audio.next_note(tick, biome, row) -> Note?   -- pure, usually none
  |- Ffi.Audio.set_mix(...)   -- one extern call per tick
  `- Ffi.Audio.note(...)      -- only when a note is due, every 4-12s
```

Nine frames in ten cost nothing. The tenth costs one extern call, occasionally
two. Nothing on this path allocates.

The callback thread reads a single-writer parameter struct with no lock, and
slews every parameter over about 50 ms. A torn double is inaudible on a gain,
and the slew is what stops a parameter jump from clicking. G15 (main is not on
the OS main thread) does not bite: miniaudio owns its own thread and is never
called from the frame loop except to write that struct.

## The music

**Sparse ambient drift.** One voice, notes of 3–8 seconds with 1–4 seconds of
gap, so a new note lands every 4 to 12 seconds and the previous note's release
tail occasionally overlaps it. That overlap is the only polyphony the score has.
The biome sets the rate inside that range (the table below); a full row takes
between one and two and a half minutes to state.

**The row.** `Tone.row(seed)` is a Fisher-Yates shuffle of pitch classes 0..11
driven by `Noise.hash2`, so one world has one row for its whole lifetime. All
twelve classes are used before any repeats — that is the technique, and it is
also what keeps the drift from settling into a key.

**Transformations.** `Tone.transform(row, form, t)` returns prime, retrograde,
inversion or retrograde-inversion at transposition `t`. The biome chooses the
form and the transposition:

| Biome | Form | Transpose | Octave | Timbre | Notes/min |
|---|---|---|---|---|---|
| Tundra | Prime | 0 | 5 | glass (sine + high partial) | 6 |
| Taiga | Inversion | 3 | 4 | soft triangle | 8 |
| Grassland | Prime | 5 | 4 | warm triangle | 10 |
| Forest | Inversion | 7 | 3 | filtered saw, low | 10 |
| Desert | Retrograde | 2 | 5 | thin sine | 5 |
| Wetland | Retrograde-inversion | 10 | 3 | hollow square, heavy filter | 12 |
| Beach | Prime | 8 | 4 | soft sine | 8 |
| Alpine | Retrograde | 0 | 6 | glass, sparse | 5 |

**Crossing a border.** The form and transposition change only at a row boundary,
after all twelve notes have sounded. Walking into a forest therefore never cuts
a row in half; the harmonic shift arrives within at most one row, up to about a
minute later. Octave, timbre and note density ease continuously, over roughly
the same window, so the immediate audible change is one of colour and pacing
rather than of material. That is the "subtle" in the requirement: the shift is
real, but it arrives as a morph, not a cue.

**Pitch.** Twelve-tone equal temperament, `midi = 12 * (octave + 1) + class`,
`hz = 440 * 2^((midi - 69) / 12)`. Computed in March; C receives Hz.

## The ambience beds

Four beds, all noise-based, all mixed in the callback, each with one gain that
March sets per tick.

**Rain.** Bandpassed white noise, plus a sparse crackle whose density follows
the same gain. Gain is `weather intensity * Biome.precip_scale(biome) *
(1 - snow_mix)`. So a desert stays quiet under the storm that soaks a wetland,
using the multiplier the particle system already keys on — the sound and the
visible rain cannot disagree. Snow makes no rain sound: as it turns to snow the
bed fades out and the muffle fades in, which is exactly what snowfall sounds
like.

**Wind.** Low-passed brown noise with a slow amplitude swell. Gain is
`Wind.at(...) * exposure`, where exposure is the player's height above sea level
normalised against the snow line (`Chunk.sea_level()` 62, `Noise.snow_line()`
95), multiplied by the skylight at the player's block. Both terms matter: a peak
is windy, and a cave at the same altitude is not.

**Leaves.** Bandpassed noise with a faster, shallower wobble than the wind bed,
so it reads as rustle rather than as more wind. Gain is `Wind.at(...) *
leaf_fraction`. The leaf fraction comes from sampling a coarse lattice around
the player — every second block in a radius of 6, about 170 reads per tick —
counting `Chunk.is_foliage`. A full per-voxel scan is what G64 warns against;
the lattice is deliberately coarse because the answer is a gain, not geometry.
Because both beds share one wind term, the leaves cannot rustle while the peaks
are silent.

**Underwater.** A low rumble with occasional bubble blips. Gain is
`Player.eye_in_water`, eased. While it is up, the other three beds duck to near
zero — you do not hear rain from under the surface.

## The muffle

One master one-pole lowpass over the whole mix, music included. March computes
the cutoff; C slews toward it.

```
cutoff = 18000 Hz  clear
       ->  4000 Hz  at snow_mix 1.0
       ->   700 Hz  eye_in_water     (dominates; underwater wins over snow)
```

Snow and water interpolate the same filter, and the underwater value takes
precedence rather than compounding. The cutoff falls monotonically as either
condition strengthens — a property the tests state directly. Muffling the music
too is deliberate: a bright clear melody under water breaks the submersion the
screen tint is selling.

## Verification

**Pure property tests.** Every decision above lives in a pure module, so it is
testable without a device:

- `test/tone_test.march` — `Tone.row(seed)` is a permutation of 0..11 for many
  seeds; each transformation is also a permutation; retrograde of retrograde is
  the identity; inversion of inversion is the identity; transposition is mod 12.
- `test/wind_test.march` — `Wind.at` stays in 0..1; it is continuous (bounded
  change per tick, the way `Weather.ease_rate` is stated); higher weather
  intensity raises its mean over a long run.
- `test/audio_test.march` — every gain stays in 0..1 for all combinations of the
  inputs; rain gain is zero when `snow_mix` is 1; every bed but the underwater
  one is near zero when the eye is in water; cutoff is monotonically
  non-increasing in `snow_mix` and lower still underwater; the biome table
  covers all eight ids.

**WAV dump.** `CF_AUDIO_DUMP=<path>` tees the mix to a 16-bit WAV, mirroring
`CF_DUMP`/`cf_gfx_dump_bmp`. With `CF_AUDIO=null` the shim opens no device and
drives the mixer from a fixed-step loop instead, so a scripted headless run
(`CF_AUTOWALK`, `CF_WEATHER`, `CF_AUTOSWIM`) produces a deterministic file you
can listen to — the audio equivalent of the BMP dumps.

**Knobs.** `CF_AUDIO=0` disables audio entirely and is the default when no
window opened, so `forge test` and headless CI never touch a sound device.
`CF_AUDIO_MUSIC=0..100` and `CF_AUDIO_AMB=0..100` pin the two master gains for
listening to one half at a time.

## What this does not do

No footsteps, no block-break sounds, no creature sound, no distance attenuation
or panning of anything — every bed is mono-positioned around the listener. No
day/night variation in the score. No reverb. Each is a separate feature and none
is needed for the requirement as stated.
