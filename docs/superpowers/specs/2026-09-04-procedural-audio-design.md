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
| `CubeForge.Tone` | The twelve-tone row: seeded generation, the four transformations, transposition, pitch class to Hz. The score states the prime only, for the world's key and its chord progression |
| `CubeForge.Score` | Musical structure: modes, the bar's metre and rhythm, the chord progression, the melodic line |
| `CubeForge.Wind` | Gust strength and direction as a pure function of `(tick, seed, weather)` |
| `CubeForge.Audio` | Mixer policy: world state to bed gains, master cutoff, the biome table, and the envelope, gain and pan of every voice |
| `CubeForge.Ffi.Audio` | Thin extern wrappers; a new `Audio` capability domain |
| `native/cf_audio.c` | The miniaudio device and the 48 kHz callback: 24 scheduled voices on a stereo delay bus, four beds, one filter |

`miniaudio.h` is vendored under `native/` (single header, public-domain/MIT
dual-licensed, no new link dependencies beyond the system frameworks it already
selects at compile time). `forge.toml`'s `[ffi] sources` gains `native/cf_audio.c`.

`Wind` is deterministic and stateless — two octaves of `Noise.hash2` over the
tick counter, smoothed. It gusts, it is seeded, it is the single source of truth
for both the mountain bed and the leaf bed, and it needs nothing threaded
through `Frame`. If precipitation later wants a slant, the same function
answers.

It is strength only. A direction was in the first draft of this design and is
not in the code: nothing consumes one. Add it when something does.

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
  |- Audio.due(sched, tick)                      -- pure; true once a bar
  |- Ffi.Audio.set_mix(...)   -- one extern call per tick
  `- Ffi.Audio.note(...) x N  -- a whole bar at once, when a bar falls due:
                                 3 pad + 1-2 bass + 1-8 melody, each with the
                                 delay that places it on its eighth
```

Nine frames in ten cost nothing. The tenth costs one extern call, occasionally
two. Nothing on this path allocates.

The callback thread reads a single-writer parameter struct with no lock, and
slews every parameter over about 50 ms. A torn double is inaudible on a gain,
and the slew is what stops a parameter jump from clicking. G15 (main is not on
the OS main thread) does not bite: miniaudio owns its own thread and is never
called from the frame loop except to write that struct.

## The music

*Rewritten 2026-09-08. What is described here replaced a first version whose
score was one voice playing a twelve-tone row, a note every four to twelve
seconds, each with a 1.2 s attack and a 2 s release. Every one of those choices
is defensible alone; together they guarantee the one thing music is not, which
is a sequence of unrelated sustained pitches. There was no pulse to hear the
notes against, no harmony to hear them in, and no shape to any single one. The
section below is what it takes for a generative score to sound like a score.*

**The unit is a bar, not a note.** The frame loop asks the score once a tick;
on the tick a bar falls due it reads a whole bar out of `Score` and pushes every
note of it at once, each carrying the delay that places it on its eighth. The
synthesiser counts that delay down per sample, so the pulse is exact even though
the loop that emitted it runs at 6 Hz. Without this the rhythmic grid could only
ever be the tick grid, which is the deeper reason the first version had no
rhythm at all.

**A bar holds three voices.**

| Voice | What it plays | Envelope | Register |
|---|---|---|---|
| Pad | the bar's triad, three detuned saws under a lowpass | attack ⅓ bar, holds, releases over ½ bar | one octave above the tonic, biome octave − 1 |
| Bass | the chord root on the downbeat, and on beat three when the biome is busy | attack 30 ms, decays to 0.45, releases over 0.6 s | octave 3, fixed — a floor does not move with the biome |
| Melody | up to eight eighths on the bar's grid | attack 12 ms, decays to 0.35, releases over 0.9 s | about 1½ octaves from the biome's octave |

Three envelopes rather than one is the single change that most decides whether
a note is heard as a note or as a key being held.

**Metre.** Eight eighth-note slots to a bar, four beats. A bar's length is given
in *ticks*, not in beats per minute, so that it is a whole number of them: at 6
ticks a second a bar of 3.4 ticks would leave the downbeat drifting by up to a
sixth of a second, and a wandering downbeat is the one rhythmic error an ear
will not forgive. 18 to 32 ticks is 80 down to 45 bpm.

**Rhythm.** The downbeat is always struck. Every other slot is struck with a
probability of its metric weight (1.0, 0.48, 0.72, 0.48, 0.9 …) times the
biome's density. A note is held to the next onset and no further, so two notes
on consecutive eighths are two notes. The last bar of every second phrase is a
cadence and thins to the strong beats: the rests are what phrase the line.

**Harmony.** A chord is a scale degree with two more stacked on it in thirds
*of the mode*, so it is in key by construction with no chord table: stack thirds
on the tonic of aeolian and a minor triad falls out, on ionian a major one, on a
pentatonic a stacked fourth that sits under either. Both the chord and the bass
are voiced by pitch class into a fixed octave rather than stacked upward from
wherever the progression is — otherwise a chord on the sixth degree lands a
sixth above the one on the tonic, and the "bass" becomes a tenor.

**The progression.** Four bars to a phrase. Bar 0 is always the tonic and the
last bar leans on the fifth degree; the two between come from the world's
twelve-tone row, one phrase at a time. That is the row's job now — it still
gives every world its own harmonic identity, over degrees that cannot be out of
key. `Tone` itself is unchanged.

**Melody.** Slots 0 and 4 take a chord tone: slot 0 the one nearest the phrase's
arc, slot 4 the one nearest where a step would have gone, so the line lands on
the harmony without standing still to reach it. Everything between moves by one
or two scale degrees, biased toward an arc that rises across the phrase and
comes home. The bias is the point: a random walk of equal steps has no shape,
because every step is as likely as its opposite — it wanders to whichever end of
the register it reaches first and stays there, which is what the first pass at
this melody did, and it is visible in `test/score_test.march` as the "is a line
rather than one note repeated" case. At the edge of the register the line is
displaced by an octave rather than clamped; clamping makes a line that has run
to the top sit on the top note repeating itself.

**The biome table.** The mode is most of what makes a biome sound like itself:
the flat second of phrygian is a desert and the sharp fourth of lydian is not.

| Biome | Mode | Key + | Octave | Melody timbre | Bar (ticks) | Density |
|---|---|---|---|---|---|---|
| Tundra | major pentatonic | 0 | 5 | glass | 30 | 0.40 |
| Taiga | aeolian | 3 | 4 | triangle | 24 | 0.55 |
| Grassland | ionian | 5 | 5 | triangle | 20 | 0.70 |
| Forest | dorian | 7 | 4 | filtered saw | 22 | 0.65 |
| Desert | phrygian | 2 | 5 | sine | 30 | 0.35 |
| Wetland | aeolian | 10 | 4 | hollow square | 18 | 0.80 |
| Beach | mixolydian | 8 | 5 | sine | 20 | 0.60 |
| Oasis | lydian | 2 | 5 | sine | 20 | 0.65 |
| Grove | minor pentatonic | 10 | 4 | hollow square | 32 | 0.35 |
| Alpine | major pentatonic | 0 | 5 | glass | 28 | 0.40 |

The melody octave stops at 5. This is the top of a three-voice texture now, not
a lone sine, and an octave higher puts the reach of the register above 3 kHz,
where a melody is a whistle. `test/audio_test.march` pins that: bass, chord and
melody all in register, in every mode, in every key, on every degree the
progression can reach — the check that caught the bass climbing with the chord
and the pad stacking itself out of the top of the mix.

**Crossing a border.** The mode and the key change only at a phrase boundary,
after the progression has come home — walking into a forest never modulates
halfway through a cadence, and the shift arrives within at most four bars. The
octave, timbre, tempo and density are the colour and pacing, and they take
effect at the next bar. That is the "subtle" in the requirement: the shift is
real, and it arrives as a modulation rather than as a cue.

**The world's key.** `Score.key_of(seed)` is the first note of the world's row,
so two worlds standing in the same biome are still in different keys. It is
folded to the nearer tonic for the bass, which would otherwise sit most of an
octave higher in a world in B than in a world in C.

**On the bus.** The three voices are panned — the chord spread wide, the bass
dead centre, the melody near the middle — and go through a ping-pong delay whose
feedback is lowpassed, so the repeats darken as they fade and drift across the
field. Notes this sparse need a tail to sound like they are in a place rather
than in a list. The ambience beds are mono and do not go through it.

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
the player — every second block in a radius of 6, 343 reads per tick — counting
`Chunk.is_foliage`. A full per-voxel scan is what G64 warns against; the lattice
is deliberately coarse because the answer is a gain, not geometry.

`full_canopy()`, the share of that box which counts as a full canopy, is 0.045
and is a measurement rather than a judgement: standing under a generated forest
fills about 2.5% of the lattice, because most of a cube around a player is air
and ground. The first draft asked for 30% and the leaf bed was inaudible in
every forest in the world.
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
       ->  4000 Hz  at snowfall 1.0
       ->   700 Hz  eye_in_water     (dominates; underwater wins over snow)
```

The filter is driven by *snowfall*, which is `weather intensity * snow_mix`,
not by `snow_mix` alone. `snow_mix` is a property of temperature: it reads 1.0
on a bright still morning in the tundra. Keying the muffle on it directly meant
a clear cold day sounded like a blizzard — caught by running a dump at a cold
seed, and now pinned by a test.

Snow and water interpolate the same filter, and the underwater value takes
precedence rather than compounding. The cutoff falls monotonically as either
condition strengthens — a property the tests state directly. Muffling the music
too is deliberate: a bright clear melody under water breaks the submersion the
screen tint is selling.

## Verification

**Pure property tests.** Every decision above lives in a pure module, so it is
testable without a device:

- `test/score_test.march` — every mode is a rising scale from the tonic and its
  octave closes; a degree below the tonic folds the way a degree above it does;
  the melody moves by steps and stays in its register; a busier density really
  is more notes and a cadence bar really is thinner; a note ends before its
  successor starts; every phrase opens on the tonic; the bass stays under the
  tonic on every chord.
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
  answers for every id; bass, chord and melody all land in register in every
  mode, key and degree; the bar line stays on the grid and the mode and key
  never change mid-phrase.

**WAV dump.** `CF_AUDIO_DUMP=<path>` tees the mix to a 16-bit WAV, mirroring
`CF_DUMP`/`cf_gfx_dump_bmp`. With `CF_AUDIO=null` the shim opens no device and
drives the mixer from a fixed-step loop instead, so a scripted headless run
(`CF_AUTOWALK`, `CF_WEATHER`, `CF_AUTOSWIM`) produces a deterministic file you
can listen to — the audio equivalent of the BMP dumps.

**Knobs.**

- `CF_AUDIO` — 0 off, 1 a real device (the default), 2 the null device.
- `CF_AUDIO_DUMP=<path>` — tee the mix to a 16-bit stereo WAV.
- `CF_AUDIO_LOG=1` — one line a second giving every bed gain, the cutoff and the
  inputs behind them, plus a line per bar with its number, biome, mode, key and
  chord degree, and a line per melody note with its slot, degree, MIDI note and
  hold. Reading what the mixer was asked for beats
  inferring it back out of a spectrum, and it is how the two bugs above were
  found.
- `CF_AUTODIVE=1` — spawns over water like `CF_AUTOSWIM` but holds the sink key
  instead of the swim key, so the eye ends up under the surface. Added because
  there was otherwise no headless way to reach the underwater state at all, and
  therefore no way to check the bed that plays there.

(While this branch was open, the app needed `MARCH_PIN_MAIN=1` on macOS or it
segfaulted before the frame loop, with or without audio — G15. The shim now
sets that itself from a C constructor, so the runs above need no such prefix.)

## Levels

The four beds are brought to about the same RMS at full gain by per-bed trims in
`cf_audio.c`, and those trims are arithmetic rather than taste. A brown-noise
integrator `y = a*y + b*n` has stationary standard deviation
`b*sd(n)/sqrt(1 - a^2)`, which for the wind and water beds is roughly ten times
what round-number multipliers assumed. The first working version measured -5.5
dBFS RMS, pinned against the limiter for the whole run. It now sits at about
-18 dBFS RMS with -6 dBFS peaks in a storm, which is where ambience belongs.

## What this does not do

No footsteps, no block-break sounds, no creature sound, no distance attenuation
or panning of anything — every bed is mono-positioned around the listener. No
day/night variation in the score. No reverb. Each is a separate feature and none
is needed for the requirement as stated.
