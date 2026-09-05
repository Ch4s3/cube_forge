/* cf_audio.c — the synthesiser. The second, and only other, unsafe surface.
 *
 * Responsibilities, and nothing else:
 *   - own a miniaudio playback device and its callback thread
 *   - turn six numbers a tick into 48 kHz stereo: one polyphonic voice,
 *     four noise beds, one master lowpass
 *   - optionally tee the mix to a 16-bit WAV
 *
 * No policy. Which notes, how loud each bed is and where the filter sits are
 * decided in March (lib/cube_forge/audio.march) and arrive here as numbers.
 * What lives here is the per-sample loop, and it lives here for the reason
 * Precip's particle loop does: March's per-element NativeArray writes are O(n)
 * on a shared array (GAPS G68), so 48,000 of them a second is not on offer.
 *
 * Threading. miniaudio owns the audio thread; the render thread never blocks
 * on it and never waits for it. The two share exactly two things:
 *   - a parameter block of _Atomic doubles, single-writer (the render thread),
 *     read once per callback and slewed toward over ~50 ms. A parameter can
 *     therefore never click, and a read that crosses a write is at worst one
 *     callback stale.
 *   - a single-producer/single-consumer note ring, 32 deep, with atomic
 *     indices. The render thread only ever pushes; the audio thread only ever
 *     pops. Neither ever takes a lock, so the audio thread cannot be made to
 *     wait on the render thread and glitch.
 *
 * CF_AUDIO=null opens no device at all: the mixer is driven synchronously from
 * cf_aud_set_mix, one tick of samples per call, straight into the WAV. A
 * headless run is then deterministic, which is what makes the dump a test
 * artefact rather than a recording.
 */
#define MA_NO_ENCODING
#define MA_NO_DECODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include "march_ffi.h"
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>

#define CF_SR         48000     /* sample rate */
#define CF_VOICES     4         /* enough for a note plus the tail of the last */
#define CF_NOTE_RING  32
#define CF_SLEW_TAU   0.05      /* seconds for a parameter to reach its target */

/* Per-bed output trims. These are not taste, they are arithmetic: each bed's
 * generator has its own stationary variance, and these bring all four to the
 * same ~0.06 RMS at full gain, which is about -24 dBFS. Ambience wants to sit
 * far below full scale — the first version of this file used round numbers
 * instead and drove the mix to -5 dBFS, pinned against the limiter.
 *
 * A brown-noise integrator y = a*y + b*n has stationary sd b*sd(n)/sqrt(1-a^2),
 * which is where the two large corrections came from: the wind and water beds
 * were roughly ten times too loud. */
#define CF_RAIN_TRIM   0.25
#define CF_WIND_TRIM   0.55
#define CF_LEAF_TRIM   0.20
#define CF_WATER_TRIM  0.90
#define CF_VOICE_TRIM  0.40

/* ── Shared with the audio thread ──────────────────────────────────────────── */

typedef struct { _Atomic double rain, wind, leaves, water, cutoff, music; } cf_params;
static cf_params g_target = {0, 0, 0, 0, 18000.0, 0};

typedef struct { double hz, dur, gain; int timbre; } cf_note;
static cf_note              g_ring[CF_NOTE_RING];
static _Atomic unsigned     g_ring_w = 0;    /* written by the render thread */
static _Atomic unsigned     g_ring_r = 0;    /* written by the audio thread  */

static ma_device g_dev;
static int       g_have_dev = 0;
static int       g_null_mode = 0;
static int       g_running   = 0;

/* ── Audio-thread-only state ───────────────────────────────────────────────── */

typedef struct {
    int    on;
    double hz, gain, phase, dur;
    int    timbre;
    double t;          /* seconds since the note started */
    double lp;         /* per-voice one-pole state, for the filtered timbres */
} cf_voice;

static struct {
    double   rain, wind, leaves, water, cutoff, music;  /* slewed, current */
    cf_voice v[CF_VOICES];
    uint32_t rng;
    double   brown_w, brown_l;   /* wind and water brown-noise integrators */
    double   rain_lp, rain_hp;
    double   leaf_lp, leaf_hp;
    double   wind_lp, water_lp;
    double   lfo_w, lfo_l;       /* bed amplitude wobble phases */
    double   crackle;            /* rain crackle envelope */
    double   bubble, bubble_ph;  /* underwater blip envelope and phase */
    double   master_lp;
} g_s = { 0, 0, 0, 0, 18000.0, 0, {{0}}, 0x1234567u, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

/* ── WAV tee ───────────────────────────────────────────────────────────────────
 * Honest caveat: with a real device open, this fwrite happens on the audio
 * thread, and fwrite can block. That is not real-time safe and could underrun.
 * It is left as it is because the tee exists for CF_AUDIO=2, where there is no
 * device and no audio thread at all — the render thread does the writing, at
 * its own pace, which is what makes the dump reproducible. Teeing a live device
 * is a convenience for listening, not a supported recording path; if it ever
 * needs to be one, the fix is a lock-free ring drained by a writer thread. */

static FILE    *g_wav = NULL;
static uint32_t g_wav_frames = 0;

static void wav_put32(FILE *f, uint32_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); fputc((v >> 16) & 255, f); fputc((v >> 24) & 255, f); }
static void wav_put16(FILE *f, uint16_t v) { fputc(v & 255, f); fputc((v >> 8) & 255, f); }

static void wav_open(const char *path) {
    g_wav = fopen(path, "wb");
    if (!g_wav) return;
    g_wav_frames = 0;
    fwrite("RIFF", 1, 4, g_wav); wav_put32(g_wav, 0); fwrite("WAVE", 1, 4, g_wav);
    fwrite("fmt ", 1, 4, g_wav); wav_put32(g_wav, 16); wav_put16(g_wav, 1); wav_put16(g_wav, 2);
    wav_put32(g_wav, CF_SR); wav_put32(g_wav, CF_SR * 4); wav_put16(g_wav, 4); wav_put16(g_wav, 16);
    fwrite("data", 1, 4, g_wav); wav_put32(g_wav, 0);
}

static void wav_close(void) {
    if (!g_wav) return;
    uint32_t data = g_wav_frames * 4;
    fseek(g_wav, 4, SEEK_SET);  wav_put32(g_wav, 36 + data);
    fseek(g_wav, 40, SEEK_SET); wav_put32(g_wav, data);
    fclose(g_wav); g_wav = NULL;
}

/* ── Synthesis ─────────────────────────────────────────────────────────────── */

/* xorshift32: the beds are noise, and noise wants to be cheap, not Gaussian. */
static inline double cf_noise(void) {
    g_s.rng ^= g_s.rng << 13; g_s.rng ^= g_s.rng >> 17; g_s.rng ^= g_s.rng << 5;
    return (double)(int32_t)g_s.rng / 2147483648.0;   /* -1 .. 1 */
}

/* One-pole lowpass coefficient for a cutoff in Hz. */
static inline double cf_k(double hz) {
    double x = exp(-6.283185307 * hz / (double)CF_SR);
    return 1.0 - x;
}

static void cf_note_on(double hz, double dur, int timbre, double gain) {
    int slot = -1;
    double oldest = -1.0;
    for (int i = 0; i < CF_VOICES; i++) {
        if (!g_s.v[i].on) { slot = i; break; }
        if (g_s.v[i].t > oldest) { oldest = g_s.v[i].t; slot = i; }   /* steal the longest-running */
    }
    cf_voice *v = &g_s.v[slot];
    v->on = 1; v->hz = hz; v->dur = dur; v->gain = gain; v->timbre = timbre;
    v->phase = 0.0; v->t = 0.0; v->lp = 0.0;
}

/* Attack 1.2 s, full for the note's length, release 2.0 s. Sparse ambient
 * drift: nothing here is meant to have an edge on it. */
static double cf_env(const cf_voice *v) {
    const double A = 1.2, R = 2.0;
    if (v->t < A) return v->t / A;
    if (v->t < v->dur) return 1.0;
    double r = (v->t - v->dur) / R;
    return r >= 1.0 ? 0.0 : (1.0 - r);
}

static double cf_osc(cf_voice *v) {
    double p = v->phase;                 /* 0..1 */
    double raw;
    switch (v->timbre) {
        case 0:  /* glass: sine plus a quiet third partial */
            raw = sin(6.283185307 * p) + 0.25 * sin(6.283185307 * 3.0 * p);
            raw *= 0.8;
            break;
        case 1:  /* triangle */
            raw = 4.0 * fabs(p - 0.5) - 1.0;
            break;
        case 2:  /* saw, lowpassed to take the fizz off */
            raw = 2.0 * p - 1.0;
            v->lp += cf_k(v->hz * 2.5) * (raw - v->lp);
            raw = v->lp;
            break;
        case 4:  /* square, filtered hard: hollow rather than buzzy */
            raw = p < 0.5 ? 1.0 : -1.0;
            v->lp += cf_k(v->hz * 1.5) * (raw - v->lp);
            raw = v->lp * 1.4;
            break;
        default: /* sine */
            raw = sin(6.283185307 * p);
            break;
    }
    v->phase += v->hz / (double)CF_SR;
    if (v->phase >= 1.0) v->phase -= 1.0;
    return raw;
}

/* Drain any notes the render thread queued. Called once per callback, not per
 * sample: a note landing up to a buffer late is inaudible at this pace. */
static void cf_drain_notes(void) {
    unsigned r = atomic_load_explicit(&g_ring_r, memory_order_relaxed);
    unsigned w = atomic_load_explicit(&g_ring_w, memory_order_acquire);
    while (r != w) {
        cf_note n = g_ring[r % CF_NOTE_RING];
        cf_note_on(n.hz, n.dur, n.timbre, n.gain);
        r++;
    }
    atomic_store_explicit(&g_ring_r, r, memory_order_release);
}

static void cf_render(float *out, uint32_t frames) {
    cf_drain_notes();

    double t_rain   = atomic_load_explicit(&g_target.rain,   memory_order_relaxed);
    double t_wind   = atomic_load_explicit(&g_target.wind,   memory_order_relaxed);
    double t_leaves = atomic_load_explicit(&g_target.leaves, memory_order_relaxed);
    double t_water  = atomic_load_explicit(&g_target.water,  memory_order_relaxed);
    double t_cutoff = atomic_load_explicit(&g_target.cutoff, memory_order_relaxed);
    double t_music  = atomic_load_explicit(&g_target.music,  memory_order_relaxed);

    const double slew = 1.0 - exp(-1.0 / ((double)CF_SR * CF_SLEW_TAU));

    for (uint32_t i = 0; i < frames; i++) {
        g_s.rain   += slew * (t_rain   - g_s.rain);
        g_s.wind   += slew * (t_wind   - g_s.wind);
        g_s.leaves += slew * (t_leaves - g_s.leaves);
        g_s.water  += slew * (t_water  - g_s.water);
        g_s.music  += slew * (t_music  - g_s.music);
        g_s.cutoff += slew * (t_cutoff - g_s.cutoff);

        /* voices */
        double voice = 0.0;
        for (int v = 0; v < CF_VOICES; v++) {
            if (!g_s.v[v].on) continue;
            double e = cf_env(&g_s.v[v]);
            if (e <= 0.0 && g_s.v[v].t > g_s.v[v].dur) { g_s.v[v].on = 0; continue; }
            voice += cf_osc(&g_s.v[v]) * e * g_s.v[v].gain;
            g_s.v[v].t += 1.0 / (double)CF_SR;
        }
        voice *= g_s.music * CF_VOICE_TRIM;

        double n = cf_noise();

        /* rain: a band of hiss, plus sparse crackle so it is not a shower head */
        g_s.rain_lp += cf_k(6000.0) * (n - g_s.rain_lp);
        g_s.rain_hp += cf_k(900.0)  * (g_s.rain_lp - g_s.rain_hp);
        double rain_band = g_s.rain_lp - g_s.rain_hp;
        if (cf_noise() > 1.0 - 0.004 * g_s.rain) g_s.crackle = 1.0;
        g_s.crackle *= 0.9992;
        double rain = (rain_band * CF_RAIN_TRIM + cf_noise() * g_s.crackle * 0.1) * g_s.rain;

        /* wind: brown noise under a slow swell */
        g_s.brown_w = 0.997 * g_s.brown_w + 0.03 * n;
        g_s.wind_lp += cf_k(450.0) * (g_s.brown_w - g_s.wind_lp);
        g_s.lfo_w += 0.11 / (double)CF_SR;
        if (g_s.lfo_w >= 1.0) g_s.lfo_w -= 1.0;
        double wind = g_s.wind_lp * CF_WIND_TRIM * g_s.wind * (0.65 + 0.35 * sin(6.283185307 * g_s.lfo_w));

        /* leaves: a higher, drier band wobbling faster, so it reads as rustle
         * against the wind rather than as more of it */
        g_s.leaf_lp += cf_k(9000.0) * (cf_noise() - g_s.leaf_lp);
        g_s.leaf_hp += cf_k(2500.0) * (g_s.leaf_lp - g_s.leaf_hp);
        g_s.lfo_l += 0.9 / (double)CF_SR;
        if (g_s.lfo_l >= 1.0) g_s.lfo_l -= 1.0;
        double leaves = (g_s.leaf_lp - g_s.leaf_hp) * CF_LEAF_TRIM * g_s.leaves
                      * (0.55 + 0.45 * sin(6.283185307 * g_s.lfo_l));

        /* water: a low rumble with the occasional bubble */
        g_s.brown_l = 0.9985 * g_s.brown_l + 0.02 * cf_noise();
        g_s.water_lp += cf_k(220.0) * (g_s.brown_l - g_s.water_lp);
        if (cf_noise() > 1.0 - 0.00004 * g_s.water) { g_s.bubble = 1.0; g_s.bubble_ph = 0.0; }
        double bub = 0.0;
        if (g_s.bubble > 0.0001) {
            g_s.bubble_ph += (300.0 + 900.0 * (1.0 - g_s.bubble)) / (double)CF_SR;
            if (g_s.bubble_ph >= 1.0) g_s.bubble_ph -= 1.0;
            bub = sin(6.283185307 * g_s.bubble_ph) * g_s.bubble * 0.25;
            g_s.bubble *= 0.9997;
        }
        double water = (g_s.water_lp * CF_WATER_TRIM + bub * 0.3) * g_s.water;

        /* master: one lowpass over everything, which is the muffle */
        double mixdown = voice + rain + wind + leaves + water;
        g_s.master_lp += cf_k(g_s.cutoff) * (mixdown - g_s.master_lp);
        double s = g_s.master_lp;

        /* soft clip: a storm on a peak can stack four beds, and a hard clip on
         * noise sounds like a fault rather than like loudness */
        s = tanh(s * 0.9);

        out[i * 2 + 0] = (float)s;
        out[i * 2 + 1] = (float)s;
    }

    if (g_wav) {
        for (uint32_t i = 0; i < frames; i++) {
            for (int c = 0; c < 2; c++) {
                double v = out[i * 2 + c];
                if (v > 1.0) v = 1.0; if (v < -1.0) v = -1.0;
                int16_t q = (int16_t)(v * 32767.0);
                fputc(q & 255, g_wav); fputc((q >> 8) & 255, g_wav);
            }
        }
        g_wav_frames += frames;
    }
}

static void cf_data_callback(ma_device *dev, void *out, const void *in, ma_uint32 frames) {
    (void)dev; (void)in;
    cf_render((float *)out, frames);
}

/* ── The March-facing surface ──────────────────────────────────────────────── */

/* mode 0: open a real device. mode 1: open none — the mixer is then driven
 * synchronously from cf_aud_set_mix, which is what makes a headless dump
 * reproducible. Returns 1 on success. */
int64_t cf_aud_init(int64_t mode) {
    if (g_running) return 1;
    g_null_mode = (mode == 1);
    if (g_null_mode) { g_running = 1; return 1; }

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.sampleRate        = CF_SR;
    cfg.dataCallback      = cf_data_callback;
    if (ma_device_init(NULL, &cfg, &g_dev) != MA_SUCCESS) return 0;
    if (ma_device_start(&g_dev) != MA_SUCCESS) { ma_device_uninit(&g_dev); return 0; }
    g_have_dev = 1; g_running = 1;
    return 1;
}

void cf_aud_close(void) {
    if (g_have_dev) { ma_device_uninit(&g_dev); g_have_dev = 0; }
    wav_close();
    g_running = 0;
}

/* One tick's worth of parameters. In null mode this also renders the tick. */
void cf_aud_set_mix(double rain, double wind, double leaves, double water,
                    double cutoff, double music) {
    if (!g_running) return;
    atomic_store_explicit(&g_target.rain,   rain,   memory_order_relaxed);
    atomic_store_explicit(&g_target.wind,   wind,   memory_order_relaxed);
    atomic_store_explicit(&g_target.leaves, leaves, memory_order_relaxed);
    atomic_store_explicit(&g_target.water,  water,  memory_order_relaxed);
    atomic_store_explicit(&g_target.cutoff, cutoff, memory_order_relaxed);
    atomic_store_explicit(&g_target.music,  music,  memory_order_release);

    if (g_null_mode) {
        static float buf[CF_SR / 6 * 2];
        cf_render(buf, CF_SR / 6);
    }
}

/* Queue one note. Single producer; the audio thread only ever reads. If the
 * ring is somehow full the note is dropped rather than blocking the render
 * thread — at one note every four seconds it never will be. */
void cf_aud_note(double hz, double dur, int64_t timbre, double gain) {
    if (!g_running) return;
    unsigned w = atomic_load_explicit(&g_ring_w, memory_order_relaxed);
    unsigned r = atomic_load_explicit(&g_ring_r, memory_order_acquire);
    if (w - r >= CF_NOTE_RING) return;
    g_ring[w % CF_NOTE_RING] = (cf_note){ hz, dur, gain, (int)timbre };
    atomic_store_explicit(&g_ring_w, w + 1, memory_order_release);
}

int64_t cf_aud_dump_begin(march_value path) {
    march_slice t = march_str_borrow(path);
    char name[512]; size_t n = t.len < 511 ? t.len : 511;
    memcpy(name, t.ptr, n); name[n] = 0;
    wav_open(name);
    return g_wav ? 1 : 0;
}

void cf_aud_dump_end(void) { wav_close(); }
