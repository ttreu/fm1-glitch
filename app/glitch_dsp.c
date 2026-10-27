/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* GLITCH audio path: a sample-accurate polymetric step sequencer with an
 * event queue (ratchets, micro-timing, swing, scatter), twelve voices of
 * synthesized microsounds, the master effects and the momentary stutter /
 * repeat engine. Audio owner, SRAM, no libc or Flash code: no division or
 * modulo by variables, no 64-bit shifts by variables, no struct copies. */
#ifndef FM1_HOST_TEST
#pragma code_seg(".volatile_ram_code")
#pragma const_seg(".volatile_ram.fm1")
#endif
#include "glitch_internal.h"

enum { BLOCK = FM1_BLOCK_FRAMES, EVENTS = 128, REC = 8192, REC_MASK = REC - 1 };

struct g_slot g_live[G_SLOTS], g_staged[G_SLOTS];
volatile uint8_t g_staged_ready;
volatile uint32_t g_staged_seed;
volatile uint32_t g_mutes;
volatile uint8_t g_run;
volatile uint8_t g_position[G_SLOTS];
volatile uint32_t g_bars, g_activity, g_seed = 1U;
volatile float g_level = 0.6f, g_hiss, g_swing, g_accent, g_flam;
volatile uint8_t g_perform_now, g_punch_now;
struct g_queue g_auditions;
struct fx_patch fx_patches[2];
volatile uint8_t fx_patch_index;

static float bus_l[BLOCK], bus_r[BLOCK], tape_send[BLOCK], rev_send[BLOCK];
static float direct_l[BLOCK], direct_r[BLOCK]; /* hard JUMP hits: past echo and reverb, 100 % on one side */

struct event { uint32_t time; uint8_t slot, velocity; };
static struct event events[EVENTS];
static unsigned event_count;
struct voice {
    uint8_t active, type, slot, highpass, reverse, direct;
    int delay, count, length, repeats, hold, width, gap;
    float env, decay, gl, gr, tape, reverb;
    float phase, phase2, freq, ratio, index, sweep, tone, amp, held, color, spacing;
    float y1, y2, lp, a1, a2, gain, lowcut, inv_length;
    uint32_t noise, lfsr;
};
static struct voice voices[G_VOICES];
static uint32_t now, random_state = 0x9e3779b9U, hiss_noise = 0x1234567U;
static float tick_phase, bar_lfo, hiss_hp[2];
static unsigned tick, was_running;   /* 1/64 ticks: 64 per bar */
static uint8_t sub[G_SLOTS], pos[G_SLOTS], pingpong[G_SLOTS];
/* DROP: a dropout of the whole bus (effect tails included). */
static float drop_depth, drop_dark, drop_level, drop_lp[2];
static int drop_hold;

FX_INLINE uint32_t rnd(void) { random_state = random_state * 1664525U + 1013904223U; return random_state; }
FX_INLINE float frand(void) { return (float)(rnd() >> 8) * (1.0f / 16777216.0f); }   /* 0..1 */
FX_INLINE unsigned pick(unsigned n) /* 0..n-1 without a division helper */
{
    unsigned i = (unsigned)(frand() * (float)n);
    return i < n ? i : n - 1U;
}
FX_INLINE float bipolar(uint32_t *s) { *s = *s * 1664525U + 1013904223U; return (float)(int32_t)*s * (1.0f / 2147483648.0f); }
FX_INLINE float sine(float t) /* sin(2 pi t), t in [0,1) */
{
    float x = t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t;
    x *= 1.5707963f;
    float x2 = x * x;
    return x * (1.0f - x2 * (0.16666667f - x2 * (0.0083333f - x2 * 0.000198f)));
}
FX_INLINE float wrap1(float t) { return t >= 1.0f ? t - (float)(int)t : t; }
FX_INLINE float soft(float x)
{
    x = fx_clamp(x, -1.5f, 1.5f);
    return x - x * x * x * (4.0f / 27.0f);
}
/* Per-sample multiplier reaching -60 dB after `seconds`. */
FX_INLINE float decay_for(float seconds) { return fx_exp2(-9.965784f / (seconds * FX_RATE)); }
/* Bit n of a 64-step word, without a 64-bit shift helper. */
FX_INLINE unsigned step_on(const struct g_slot *s, unsigned n)
{
    const volatile uint32_t *w = (const volatile uint32_t *)&s->steps;
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    return n < 32U ? (w[1] >> n) & 1U : (w[0] >> (n - 32U)) & 1U;
#else
    return n < 32U ? (w[0] >> n) & 1U : (w[1] >> (n - 32U)) & 1U;
#endif
}

static FM1_AUDIO_CODE void schedule(uint32_t time, unsigned slot, unsigned velocity)
{
    if (event_count >= EVENTS) return;
    events[event_count].time = time;
    events[event_count].slot = (uint8_t)slot;
    events[event_count].velocity = (uint8_t)velocity;
    ++event_count;
}
static FM1_AUDIO_CODE void trigger(unsigned slot, unsigned velocity, int delay)
{
    if (slot >= G_SLOTS) return;
    const uint8_t *s = g_live[slot].value;
    unsigned source = s[GP_SOURCE] < GS_COUNT ? s[GP_SOURCE] : GS_IMPULSE;
    float level = (float)s[GP_LEVEL] * (1.0f / 99.0f);
    if (level <= 0.0f) return;
    float random = (float)s[GP_RANDOM] * (1.0f / 99.0f);
    /* RANDOM varies each hit a little (up to +-3 semitones, +-12 % length). */
    float pitch = fx_exp2(((float)s[GP_PITCH] - 24.0f + random * 3.0f * (2.0f * frand() - 1.0f)) * (1.0f / 12.0f));
    float shape = fx_clamp((float)s[GP_DECAY] * (1.0f / 99.0f) * (1.0f + random * 0.24f * (frand() - 0.5f)), 0.0f, 1.0f);
    float color = (float)s[GP_COLOR] * (1.0f / 99.0f);
    unsigned reverse = s[GP_REVERSE] && frand() * 100.0f < (float)s[GP_REVERSE];
    const unsigned punch = g_punch_now;
    if (punch == GX_REVERSE) reverse = 1;
    if (punch == GX_CHAOS) { /* up to an octave either way, every other hit backwards */
        pitch *= fx_exp2((float)((int)pick(25) - 12) * (1.0f / 12.0f));
        reverse = (rnd() >> 9) & 1U;
    }
    float fall = 0.0f, fall_sweep = 0.0f;  /* seconds to -60 dB of the envelope and of a sweep */
    g_activity |= 1U << slot;
    if (source == GS_DROP) {
        drop_depth = level;
        drop_dark = color;
        drop_hold = delay + (int)((0.02f + 0.38f * shape) * FX_RATE);
        return;
    }
    /* Pick a voice: free first, otherwise the quietest (a swelling reversed
     * voice counts at its full level). */
    struct voice *v = &voices[0];
    float quietest = 1e9f;
    for (unsigned i = 0; i < G_VOICES; ++i) {
        if (!voices[i].active) { v = &voices[i]; break; }
        float l = (voices[i].reverse ? 1.0f : voices[i].env) * voices[i].gain;
        if (l < quietest) { quietest = l; v = &voices[i]; }
    }
    float amount = (float)s[GP_PAN] * (1.0f / 99.0f), pan = 0.0f;
    switch (s[GP_PAN_MODE]) {
    case PAN_FIXED: pan = 2.0f * amount - 1.0f; break;
    case PAN_RANDOM: pan = (rnd() & 0x100U ? 1.0f : -1.0f) * amount * (0.8f + 0.2f * frand()); break;
    case PAN_PINGPONG: pingpong[slot] ^= 1U; pan = (pingpong[slot] ? 1.0f : -1.0f) * amount; break;
    case PAN_JUMP: if (frand() < 0.5f * amount) pan = rnd() & 0x100U ? 1.0f : -1.0f; break; /* else the middle */
    default: pan = sine(bar_lfo) * amount; break;
    }
    /* Loudness per source (A-weighted, measured): the fine, short sounds get
     * more gain so they are not lost next to snaps and chirps. */
    static const float loudness[GS_COUNT] = {
        [GS_HAT] = 1.2f, [GS_IMPULSE] = 2.0f, [GS_TRAIN] = 2.2f, [GS_CRUNCH] = 2.2f, [GS_TICK] = 4.5f,
        [GS_CHIRP] = 1.0f, [GS_ZAP] = 1.1f, [GS_SNAP] = 0.7f, [GS_DUST] = 2.5f, [GS_BITS] = 3.5f,
        [GS_GRAIN] = 1.4f, [GS_THUMP] = 1.4f, [GS_POP] = 3.5f, [GS_LOOP] = 1.0f, [GS_HIGH] = 1.0f, [GS_DROP] = 1.0f};
    float gain = level * level * (float)velocity * (1.0f / 127.0f) * loudness[source];
    v->active = 1; v->slot = (uint8_t)slot; v->delay = delay; v->type = (uint8_t)source;
    v->gain = gain;
    v->gl = gain * (pan > 0.0f ? 1.0f - pan : 1.0f);
    v->gr = gain * (pan < 0.0f ? 1.0f + pan : 1.0f);
    v->tape = (float)s[GP_TAPE] * (1.0f / 99.0f);
    v->reverb = (float)s[GP_SHIMMER] * (1.0f / 99.0f);
    v->direct = s[GP_PAN_MODE] == PAN_JUMP && (pan >= 1.0f || pan <= -1.0f); /* dry: nothing on the other side */
    if (v->direct) v->tape = v->reverb = 0.0f;
    v->env = 1.0f; v->decay = 1.0f; v->amp = 1.0f; v->phase = v->phase2 = 0.0f;
    v->lp = v->y1 = v->y2 = v->lowcut = 0.0f;
    v->noise = rnd() | 1U; v->count = 0; v->color = color;
    v->highpass = source != GS_HAT && source != GS_THUMP && source != GS_POP && source != GS_LOOP; /* microsounds stay out of the bass */
    switch (source) {
    case GS_HAT:
        fall = 0.015f + 0.25f * shape * shape;
        v->decay = decay_for(fall);
        v->a1 = fx_clamp((0.15f + 0.7f * color) * pitch, 0.05f, 0.95f);
        v->length = 0x7fffffff; break;
    case GS_IMPULSE: /* 4..66 samples; COLOR adds the negative half of a doublet */
        v->length = 4 + (int)((62.0f * shape) / pitch);
        v->held = rnd() & 0x400U ? 0.9f : -0.9f; break;
    case GS_TRAIN:   /* COLOR < 50 accelerates, > 50 slows down */
        v->repeats = 3 + (int)(17.0f * shape);
        v->spacing = (0.002f + 0.010f * (1.0f - 0.7f * shape) + 0.002f * random * frand()) * FX_RATE / pitch;
        v->hold = (int)v->spacing;
        v->ratio = 0.8f + 0.4f * color;
        v->width = 2 + (int)pick(4); v->gap = 0;
        v->length = 0x7fffffff;                          /* ends after its repeats */
        v->held = rnd() & 0x400U ? 0.8f : -0.8f; break;
    case GS_CRUNCH:  /* PITCH: hold rate; COLOR: smooth steps .. harsh, jittery crunch */
        v->freq = fx_clamp(2500.0f * pitch, 200.0f, 20000.0f);
        fall = 0.006f + 0.18f * shape;
        v->decay = decay_for(fall);
        v->length = (int)(fall * 0.7f * FX_RATE);
        v->phase = 1.0f; v->spacing = 1.0f; v->held = 0.0f;
        v->amp = 0.55f; break;
    case GS_TICK:    /* COLOR: dark to bright noise */
        v->length = (int)((0.002f + 0.02f * shape) * FX_RATE);
        v->a1 = 0.05f + 0.95f * color;
        fall = (float)v->length * (1.5f / FX_RATE);
        v->decay = decay_for(fall); break;
    case GS_CHIRP:   /* COLOR: -4 octaves (down) .. +4 octaves (up) */
        v->freq = fx_clamp(700.0f * pitch, 60.0f, 12000.0f);
        v->length = (int)((0.005f + 0.08f * shape) * FX_RATE);
        v->sweep = fx_exp2((color - 0.5f) * 8.0f / (float)v->length);
        v->amp = 0.6f; break;
    case GS_ZAP:     /* COLOR: how high the fall starts */
        v->freq = 120.0f * pitch;
        v->ratio = 4.0f + 60.0f * color;
        v->tone = 1.0f;
        fall_sweep = 0.01f + 0.05f * shape;
        v->sweep = decay_for(fall_sweep);
        v->length = (int)((0.01f + 0.15f * shape) * FX_RATE);
        fall = 0.012f + 0.15f * shape;
        v->decay = decay_for(fall);
        v->amp = 0.6f; break;
    case GS_SNAP:    /* COLOR: dark .. bright band of noise; DECAY: length of the gate */
        v->a1 = fx_clamp((0.12f + 0.8f * color) * pitch, 0.02f, 0.95f); /* upper edge of the band */
        v->a2 = v->a1 * 0.12f;                                          /* lower edge */
        v->length = (int)((0.004f + 0.07f * shape) * FX_RATE);
        v->held = rnd() & 0x400U ? 1.0f : -1.0f;
        v->amp = 1.5f; break;
    case GS_DUST:    /* COLOR: density of the crackle */
        v->length = (int)((0.05f + 0.6f * shape) * FX_RATE);
        v->ratio = (60.0f + 3000.0f * color * color) * (1.0f / FX_RATE);
        v->held = 0.0f; break;
    case GS_BITS:    /* COLOR < 50: short, tonal 7-bit LFSR; above: 15-bit noise */
        v->freq = fx_clamp(1500.0f * pitch, 100.0f, 22000.0f);
        v->lfsr = color < 0.5f ? 0x7fU : 0x7fffU;
        v->length = (int)((0.005f + 0.1f * shape) * FX_RATE);
        fall = (float)v->length * (1.2f / FX_RATE);
        v->decay = decay_for(fall);
        v->amp = 0.5f; break;
    case GS_THUMP:   /* PITCH: 20-200 Hz; COLOR: pure sine .. click and drive; DECAY: a cycle .. a boom */
        v->freq = fx_clamp(42.0f * pitch, 18.0f, 200.0f);
        v->ratio = 1.4f + 2.6f * color;                   /* the start of the fall, times the end */
        v->tone = 1.0f;
        v->sweep = decay_for(0.015f + 0.03f * shape);
        v->length = (int)((0.035f + 0.45f * shape * shape) * FX_RATE); /* held, then falls (voice_sample) */
        v->phase = 0.25f;                                 /* sine() is -cos: start at the zero crossing */
        v->held = color * (rnd() & 0x400U ? 0.8f : -0.8f);
        v->amp = 0.9f; break;
    case GS_POP:     /* COLOR: soft .. sharp edge; DECAY: how slowly it slides back */
        v->held = rnd() & 0x400U ? 1.0f : -1.0f;
        v->a1 = fx_clamp((0.01f + 0.9f * color * color) * pitch, 0.003f, 1.0f);
        fall = 0.003f + 0.12f * shape * shape;
        v->decay = decay_for(fall);
        v->length = (int)(fall * 0.7f * FX_RATE) + 64;
        v->amp = 0.8f; break;
    case GS_LOOP:    /* PITCH: loop of 1-20 ms (7.2 ms at 0); COLOR: round body .. bright sizzle; DECAY: 20-300 ms */
        /* A snippet looped too short: a smooth wave with two or three bumps per
         * pass plus bright noise, repeated exactly, jumping to a slightly
         * different snippet every 3-6 passes. */
        v->hold = (int)fx_clamp(0.0072f * FX_RATE / pitch, 40.0f, 900.0f); /* whole samples: exact repeats */
        v->spacing = 1.0f / (float)v->hold;
        v->gap = 0;
        v->lfsr = rnd() | 1U;                             /* the seed of the looped noise */
        v->y1 = 0.3f * bipolar(&v->lfsr);                 /* the body: three harmonics of the pass */
        v->index = 0.7f + 0.3f * bipolar(&v->lfsr);
        v->y2 = 0.35f * bipolar(&v->lfsr);
        v->repeats = 3 + (int)pick(4);
        v->length = (int)((0.02f + 0.28f * shape * shape) * FX_RATE);
        v->amp = 0.75f; break;
    case GS_HIGH:    /* PITCH: 7.7 kHz at 0, 4-18 kHz; COLOR: pure .. a beating second tone; DECAY: 5-200 ms */
        v->freq = fx_clamp(7700.0f * pitch, 4000.0f, 18000.0f);
        v->ratio = 1.0f + 0.05f * color;                  /* the second tone, a little higher */
        v->length = (int)((0.005f + 0.2f * shape * shape) * FX_RATE);
        v->amp = 0.3f; break;
    default: {       /* GS_GRAIN: COLOR = resonance */
        float f = fx_clamp(1200.0f * pitch, 100.0f, 12000.0f), r = 1.0f - (0.003f + 0.08f * (1.0f - color));
        float c = sine(wrap1(0.25f + f * (1.0f / FX_RATE)));
        v->a1 = 2.0f * r * c; v->a2 = r * r; v->amp = (1.0f - r) * 6.0f;
        v->length = (int)((0.004f + 0.06f * shape) * FX_RATE);
        break;
    }
    }
    if (v->length < 2) v->length = 2;
    v->reverse = (uint8_t)reverse;
    if (reverse) switch (source) {
    case GS_IMPULSE: case GS_DUST: break;        /* mirrored in voice_sample */
    case GS_THUMP:                               /* the pitch rises into the cut */
        v->tone = fx_exp2(-9.965784f * (float)v->length / ((0.015f + 0.03f * shape) * FX_RATE));
        v->sweep = 1.0f / v->sweep;
        break;
    case GS_TRAIN: {                             /* quiet and wide first, loud and close last */
        float held = v->held, spacing = v->spacing;
        for (int i = 1; i < v->repeats; ++i) { held *= -0.85f; if (i > 1) spacing *= v->ratio; }
        v->held = held; v->spacing = spacing;
        v->hold = (int)fx_clamp(spacing, 8.0f, 2000.0f);
        v->ratio = 1.0f / v->ratio;
        break;
    }
    case GS_CHIRP:                               /* the sweep runs the other way */
        v->freq = fx_clamp(v->freq * fx_exp2((color - 0.5f) * 8.0f), 30.0f, 18000.0f);
        v->sweep = 1.0f / v->sweep;
        break;
    default:
        if (fall > 0.0f) {
            /* The decay turned around: a swell from -40 dB that ends at full level. */
            float n = fall * FX_RATE * 0.6667f;
            float span = (float)v->length < n ? (float)v->length : n;
            v->env = fx_exp2(-9.965784f * span / (fall * FX_RATE));
            v->decay = 1.0f / v->decay;
            if (fall_sweep > 0.0f) { v->tone = fx_exp2(-9.965784f * span / (fall_sweep * FX_RATE)); v->sweep = 1.0f / v->sweep; }
            v->length = (int)span;
        }                                        /* SNAP, GRAIN: a ramp in voice_sample */
        break;
    }
    if (v->length < 2) v->length = 2;
    v->inv_length = 1.0f / (float)v->length;
}
static FM1_AUDIO_CODE float voice_sample(struct voice *v)
{
    float y = 0.0f;
    if (v->count >= v->length) { v->active = 0; return 0.0f; }
    float x = (float)v->count * v->inv_length;     /* 0..1 through the sound */
    switch (v->type) {
    case GS_HAT: {
        float n = bipolar(&v->noise);
        float d1 = n - v->y1; v->y1 = n;              /* two differentiators */
        float d2 = d1 - v->y2; v->y2 = d1;
        v->lp += (d2 - v->lp) * v->a1;
        y = 1.4f * v->lp;
        break;
    }
    case GS_IMPULSE: {
        unsigned first = v->count < v->length / 2;
        if (v->reverse) first = !first;
        y = first ? v->held : -v->color * v->held;
        break;
    }
    case GS_TRAIN:
        if (v->gap < v->width) y = v->held;
        if (++v->gap >= v->hold) {
            v->gap = 0;
            v->held *= v->reverse ? -1.1764706f : -0.85f; /* alternate and fade (or grow) */
            v->spacing *= v->ratio;
            v->hold = (int)fx_clamp(v->spacing, 8.0f, 2000.0f);
            if (--v->repeats <= 0) { v->active = 0; return 0.0f; }
        }
        break;
    case GS_CRUNCH: {
        v->phase += v->freq * v->spacing * (1.0f / FX_RATE);
        if (v->phase >= 1.0f) {                      /* a new random level, fewer of them with COLOR */
            v->phase -= (float)(int)v->phase;
            float levels = 2.0f + 14.0f * (1.0f - v->color);
            v->held = (float)(int)(bipolar(&v->noise) * levels) / levels;
            v->spacing = fx_clamp(1.0f + 1.6f * v->color * bipolar(&v->noise), 0.2f, 2.6f);
        }
        v->lp += (v->held - v->lp) * (0.3f + 0.7f * v->color);
        y = v->amp * v->lp;
        break;
    }
    case GS_TICK: {
        float n = bipolar(&v->noise);
        v->lp += (n - v->lp) * v->a1;
        float bright = n - v->y1;
        v->y1 = n;
        y = 0.7f * (v->lp + v->color * (bright - v->lp));
        break;
    }
    case GS_CHIRP: {
        v->phase = wrap1(v->phase + v->freq * (1.0f / FX_RATE));
        v->freq = fx_clamp(v->freq * v->sweep, 30.0f, 18000.0f);
        float edge = v->reverse ? x * fx_clamp((float)(v->length - v->count) * (1.0f / 22.0f), 0.0f, 1.0f)
                                : fx_clamp((float)v->count * (1.0f / 22.0f), 0.0f, 1.0f) * (1.0f - x);
        y = v->amp * edge * sine(v->phase);
        break;
    }
    case GS_ZAP: {
        float f = v->freq * (1.0f + (v->ratio - 1.0f) * v->tone);
        v->tone = fx_clamp(v->tone * v->sweep, 0.0f, 1.0f);
        v->phase = wrap1(v->phase + fx_clamp(f, 20.0f, 18000.0f) * (1.0f / FX_RATE));
        y = v->amp * sine(v->phase) * fx_clamp((float)v->count * (1.0f / 22.0f), 0.0f, 1.0f);
        break;
    }
    case GS_SNAP: {
        float n = bipolar(&v->noise);
        v->lp += (n - v->lp) * v->a1;
        v->y1 += (v->lp - v->y1) * v->a2;
        float gate = v->reverse ? x * x : 1.0f - x * x;   /* flat, then a quick close (reversed: a ramp) */
        unsigned click = v->reverse ? v->count + 3 >= v->length : v->count < 3;
        y = (v->lp - v->y1) * v->amp * gate + (click ? v->held : 0.0f);
        break;
    }
    case GS_DUST: {
        float n = bipolar(&v->noise);
        if ((float)(v->noise >> 8) * (1.0f / 16777216.0f) < v->ratio) v->held = n > 0.0f ? 0.5f + 0.5f * n : -0.5f + 0.5f * n;
        y = v->held;
        v->held *= 0.35f;
        y *= v->reverse ? x : 1.0f - x;
        break;
    }
    case GS_BITS:
        v->phase += v->freq * (1.0f / FX_RATE);
        if (v->phase >= 1.0f) {
            v->phase -= (float)(int)v->phase;
            uint32_t l = v->lfsr, bit = (l ^ (l >> 1)) & 1U;
            v->lfsr = v->color < 0.5f ? (l >> 1) | (bit << 6) : (l >> 1) | (bit << 14);
        }
        y = v->amp * ((v->lfsr & 1U) ? 1.0f : -1.0f);
        break;
    case GS_THUMP: {
        float f = v->freq * (1.0f + (v->ratio - 1.0f) * v->tone);
        v->tone = fx_clamp(v->tone * v->sweep, 0.0f, 1.0f);
        v->phase = wrap1(v->phase + f * (1.0f / FX_RATE));
        float t = v->reverse ? 1.0f - x : x, g = 1.0f - t * t;
        y = v->amp * g * g * soft(sine(v->phase) * (1.0f + 1.5f * v->color));
        if (!v->reverse && v->count < 24) y += v->held * bipolar(&v->noise) * (1.0f - (float)v->count * (1.0f / 24.0f)); /* click */
        break;
    }
    case GS_POP:
        v->lp += (v->held - v->lp) * v->a1;              /* the edge of the jump */
        y = v->amp * v->lp;
        break;
    case GS_LOOP: {
        if (v->gap == 0) {                               /* a new pass of the loop */
            if (v->count && --v->repeats <= 0) {         /* on to a slightly different snippet */
                v->lfsr = v->lfsr * 2654435761U + 1U;
                v->y1 = fx_clamp(v->y1 + 0.15f * bipolar(&v->lfsr), -0.4f, 0.4f);
                v->y2 = fx_clamp(v->y2 + 0.15f * bipolar(&v->lfsr), -0.45f, 0.45f);
                v->repeats = 3 + (int)pick(4);
            }
            v->noise = v->lfsr;                          /* the same noise every pass */
            v->lp = 0.0f;
        }
        float p = (float)v->gap * v->spacing;
        if (++v->gap >= v->hold) v->gap = 0;
        float body = v->y1 * sine(p) + v->index * sine(wrap1(2.0f * p)) + v->y2 * sine(wrap1(3.0f * p));
        float n = bipolar(&v->noise), sizzle = n - v->lp; /* bright: the noise differentiated */
        v->lp = n;
        float gate = fx_clamp((float)v->count * (1.0f / 44.0f), 0.0f, 1.0f) *
                     fx_clamp((float)(v->length - v->count) * (1.0f / 220.0f), 0.0f, 1.0f);
        if (v->reverse) gate *= x;                       /* reversed: grows into the cut */
        y = v->amp * gate * ((1.0f - 0.7f * v->color) * body + (0.1f + 0.5f * v->color) * sizzle);
        break;
    }
    case GS_HIGH: {
        v->phase = wrap1(v->phase + v->freq * (1.0f / FX_RATE));
        v->phase2 = wrap1(v->phase2 + v->freq * v->ratio * (1.0f / FX_RATE));
        float gate = fx_clamp((float)v->count * (1.0f / 60.0f), 0.0f, 1.0f) *
                     fx_clamp((float)(v->length - v->count) * (1.0f / 130.0f), 0.0f, 1.0f);
        if (v->reverse) gate *= x;
        y = v->amp * gate * (sine(v->phase) + 0.7f * v->color * sine(v->phase2));
        break;
    }
    default: { /* GS_GRAIN */
        float n = bipolar(&v->noise);
        float out = v->amp * n + v->a1 * v->y1 - v->a2 * v->y2;
        v->y2 = v->y1; v->y1 = out;
        float w = v->reverse ? x : sine(0.5f * x);       /* sin(pi x): a Hann window; reversed a ramp */
        y = out * w * w;
        break;
    }
    }
    if (v->reverse) y *= fx_clamp((float)(v->length - v->count) * (1.0f / 32.0f), 0.0f, 1.0f); /* no click at the end */
    ++v->count;
    if (v->highpass) { v->lowcut += (y - v->lowcut) * 0.03f; y -= v->lowcut; }
    y *= v->env;
    v->env *= v->decay;
    if (v->env < 0.0005f) v->active = 0;
    return y;
}

/* ------------------------------------------------------------ sequencer */
static FM1_AUDIO_CODE void adopt_staged(void)
{
    volatile uint8_t *to = (volatile uint8_t *)g_live;
    const volatile uint8_t *from = (const volatile uint8_t *)g_staged;
    for (unsigned i = 0; i < sizeof g_live; ++i) to[i] = from[i];
    g_seed = g_staged_seed;
    for (unsigned s = 0; s < G_SLOTS; ++s) pos[s] = sub[s] = 0;
    FX_BARRIER();
    g_staged_ready = 0;
}
/* One 1/64 tick. Each slot advances at its RATE (every 4, 2 or 1 ticks)
 * through its own LENGTH, so loops of different lengths drift apart. */
static FM1_AUDIO_CODE void on_tick(uint32_t time, float samples_per_tick)
{
    if (tick == 0) {
        if (g_staged_ready) adopt_staged();
        ++g_bars;
    }
    /* Swing delays the second sixteenth of every eighth. */
    const uint32_t swing = (tick & 7U) == 4U ? (uint32_t)(g_swing * samples_per_tick * 4.0f) : 0U;
    const uint32_t mutes = g_mutes;
    const unsigned punch = g_punch_now < GX_COUNT ? g_punch_now : GX_OFF;
    for (unsigned slot = 0; slot < G_SLOTS; ++slot) {
        const struct g_slot *d = &g_live[slot];
        const uint8_t *v = d->value;
        unsigned rate = v[GP_RATE] < 3 ? v[GP_RATE] : 0, every = 4U >> rate;
        unsigned prob = v[GP_PROB], muted = (mutes >> slot) & 1U;
        float micro = (float)v[GP_MICRO] * (0.010f / 99.0f) * FX_RATE; /* up to 10 ms late */
        if (sub[slot] == 0) {
            unsigned length = v[GP_LENGTH] < 1 ? 1U : v[GP_LENGTH] > G_STEPS ? G_STEPS : v[GP_LENGTH];
            unsigned p = pos[slot] < length ? pos[slot] : 0U;
            g_position[slot] = (uint8_t)p;
            if (!muted && step_on(d, p) && frand() * 100.0f < (float)prob) {
                /* A hash of slot, step and seed: micro timing and in-between hits
                 * repeat with the loop (they sit in the beat), MUTATE moves them. */
                uint32_t h = (slot * 73856093U) ^ (p * 19349663U) ^ (g_seed * 83492791U);
                h = (h ^ (h >> 13)) * 2654435761U; h ^= h >> 16;
                float late = micro * (float)(h & 1023U) * (1.0f / 1024.0f);
                /* ACCENT: quarters full, eighths and sixteenths softer. */
                float weight = (tick & 15U) == 0 ? 1.0f : (tick & 7U) == 0 ? 0.8f : (tick & 3U) == 0 ? 0.62f : 0.5f;
                float accent = 1.0f - g_accent * (1.0f - weight);
                static const uint8_t hits[6] = {1, 2, 3, 4, 6, 5}, fade[6] = {0, 30, 20, 15, 10, 0};
                static const float gaps[6] = {1.0f, 0.5f, 1.0f / 3.0f, 0.25f, 1.0f / 6.0f, 1.0f};
                /* ROLL (index 5): accelerating hits that get louder. */
                static const float roll_at[5] = {0.0f, 0.36f, 0.64f, 0.84f, 0.96f};
                unsigned r = g_step_ratchet(d, p);       /* the step's own ratchet wins */
                if (!r) r = v[GP_RATCHET] < 5 ? v[GP_RATCHET] : 0;
                if (punch >= GX_RATCHET_2 && punch <= GX_RATCHET_4 && r < punch) r = punch; /* punch-in: at least X2..X4 */
                if (punch == GX_ROLL) r = 5;
                float length = samples_per_tick * (float)every, gap = length * gaps[r];
                for (unsigned i = 0; i < hits[r]; ++i) {
                    float at = r == 5 ? length * roll_at[i] : gap * (float)i;
                    unsigned velocity = r == 5 ? 60U + i * 16U : 127U - i * fade[r];
                    schedule(time + swing + (uint32_t)(at + late), slot, (unsigned)((float)velocity * accent));
                }
                /* FLAM: now and then the hit comes twice, the second one softer
                 * and between the steps (a 1/2, 1/4, 3/4, 1/3, 2/3 or 1/8 step later). */
                if (hits[r] == 1 && (float)((h >> 10) & 1023U) < g_flam * 400.0f) {
                    static const float between_at[8] = {0.5f, 0.25f, 0.75f, 1.0f / 3.0f, 2.0f / 3.0f, 0.125f, 0.5f, 0.25f};
                    float again = length * between_at[(h >> 20) & 7U];
                    unsigned soft_hit = 70U + ((h >> 23) & 31U);
                    schedule(time + swing + (uint32_t)(again + late), slot, (unsigned)((float)soft_hit * accent));
                }
            }
            pos[slot] = (uint8_t)(p + 1U < length ? p + 1U : 0U);
        }
        if (++sub[slot] >= every) sub[slot] = 0;
        /* Scatter: N extra hits per bar on the slot's own grid. The positions
         * come from a hash of the seed, so they repeat every bar like a hidden
         * pattern; PROB still drops some. */
        if (tick == 0 && v[GP_SCATTER] && !muted) {
            unsigned count = (unsigned)v[GP_SCATTER] << rate, grid = 16U << rate, shift = 2U - rate;
            for (unsigned i = 0; i < count && i < 96U; ++i) {
                uint32_t h = (slot * 73856093U) ^ (i * 19349663U) ^ (g_seed * 83492791U);
                h = (h ^ (h >> 13)) * 2654435761U;
                unsigned position = (h >> 7) & (grid - 1U);
                if (frand() * 100.0f < (float)prob)
                    schedule(time + (uint32_t)(samples_per_tick * (float)(position << shift) + micro * frand()),
                             slot, 70U + ((h >> 3) & 31U) + ((h >> 9) & 15U));
            }
        }
    }
}

/* ------------------------------------------------------------ perform */
/* The bus is recorded at half rate (int16 stereo, 0.37 s). Repeats and
 * reverses loop its most recent slice (recording pauses meanwhile); tape
 * stop reads it ever slower; the other effects work on the bus directly. */
static int16_t rec_l[REC], rec_r[REC];
static uint32_t rec_w, loop_start, tape_base;
static unsigned rec_half, active_fx, hold_n;
static float rec_sum[2], loop_len, loop_pos, loop_rate, tape_frac, tape_rate, wet, sweep, gate_phase;
static float hold[2], filt[2][2], gate_gain = 1.0f;
static float scratch_dir = 1.0f, stutter_base;
static uint32_t slices_start;   /* SHUFFLE: where the last beat begins; STUTTER: where the loop ends */

FX_INLINE int16_t s16(float x) { return (int16_t)fx_clamp(x * 16384.0f, -32767.0f, 32767.0f); }
FX_INLINE float rec_read(const int16_t *buf, uint32_t base, float offset)
{
    int whole = (int)offset;
    float frac = offset - (float)whole;
    uint32_t i = base + (uint32_t)whole;
    float a = buf[i & REC_MASK], b = buf[(i + 1U) & REC_MASK];
    return (a + (b - a) * frac) * (1.0f / 16384.0f);
}
FX_INLINE unsigned backwards(unsigned fx) { return fx == GF_REVERSE_4 || fx == GF_REVERSE_8 || fx == GF_REVERSE_16; }
static FM1_AUDIO_CODE void engage(unsigned fx, float samples_per_tick)
{
    static const float beats[GF_COUNT] = {
        [GF_REPEAT_4] = 1.0f, [GF_REPEAT_8] = 0.5f, [GF_REPEAT_16] = 0.25f, [GF_REPEAT_32] = 0.125f,
        [GF_REPEAT_64] = 0.0625f, [GF_REVERSE_4] = 1.0f, [GF_REVERSE_8] = 0.5f,
        [GF_REVERSE_16] = 0.25f, [GF_SCRATCH] = 0.5f, [GF_SHUFFLE] = 0.25f, [GF_OCTAVE] = 0.25f, [GF_STUTTER] = 0.25f};
    float beat = samples_per_tick * 16.0f * 0.5f;     /* one beat in half-rate frames */
    active_fx = fx;
    if ((fx >= GF_REPEAT_4 && fx <= GF_REVERSE_8) || fx >= GF_REVERSE_16) {
        loop_len = beat * beats[fx];
    } else if (fx == GF_PITCHDOWN) {
        loop_len = beat * 0.25f;
    } else if (fx == GF_FREEZE) {
        loop_len = 2048.0f;
    } else if (fx == GF_TAPESTOP) {
        tape_base = rec_w - 32U; tape_frac = 0.0f; tape_rate = 1.0f;
        return;
    } else {
        sweep = 0.0f; gate_phase = 0.0f; hold_n = 0;
        return;
    }
    /* SHUFFLE picks from four slices, STUTTER grows up to twice its base. */
    float span = fx == GF_SHUFFLE ? 4.0f : fx == GF_STUTTER ? 2.0f : 1.0f;
    while (loop_len * span > (float)(REC - 64)) loop_len *= 0.5f;
    if (loop_len < 16.0f) loop_len = 16.0f;
    loop_start = rec_w - (uint32_t)loop_len;
    slices_start = rec_w - (uint32_t)(loop_len * 4.0f);
    if (fx == GF_STUTTER) { stutter_base = loop_len; slices_start = rec_w; }
    loop_pos = backwards(fx) ? loop_len - 1.0f : 0.0f;
    loop_rate = 1.0f;
    scratch_dir = 1.0f;
}
FX_INLINE unsigned looping(unsigned fx)
{
    return (fx >= GF_REPEAT_4 && fx <= GF_FREEZE && fx != GF_TAPESTOP) || fx >= GF_REVERSE_16;
}
static FM1_AUDIO_CODE void perform(float *left, float *right, float samples_per_tick)
{
    unsigned want = g_perform_now < GF_COUNT ? g_perform_now : GF_OFF;
    if (want != GF_OFF && want != active_fx) engage(want, samples_per_tick);
    if (active_fx == GF_OFF) want = GF_OFF;
    const unsigned fx = active_fx;
    const float target = want != GF_OFF ? 1.0f : 0.0f;
    const float gate_step = 1.0f / (samples_per_tick * 2.0f); /* one 1/32 note */
    for (unsigned i = 0; i < BLOCK; ++i) {
        float l = left[i], r = right[i];
        if (!looping(fx) || wet <= 0.0f) {
            rec_sum[0] += l; rec_sum[1] += r;
            if (++rec_half >= 2U) {
                rec_l[rec_w & REC_MASK] = s16(0.5f * rec_sum[0]);
                rec_r[rec_w & REC_MASK] = s16(0.5f * rec_sum[1]);
                ++rec_w; rec_half = 0; rec_sum[0] = rec_sum[1] = 0.0f;
            }
        }
        wet += (target - wet) * 0.01f;                   /* about 2 ms in and out */
        if (fx == GF_OFF || wet < 0.0005f) { if (target == 0.0f) { wet = 0.0f; active_fx = GF_OFF; } continue; }
        float el = l, er = r;
        if (looping(fx)) {
            if (fx == GF_FREEZE) { /* two crossfaded heads: a smooth sustained loop */
                float p2 = loop_pos + 0.5f * loop_len;
                if (p2 >= loop_len) p2 -= loop_len;
                float w1 = loop_pos / loop_len, w2 = p2 / loop_len;
                w1 = 1.0f - fx_clamp(2.0f * w1 - 1.0f, -1.0f, 1.0f); w1 = w1 > 1.0f ? 2.0f - w1 : w1;
                w2 = 1.0f - fx_clamp(2.0f * w2 - 1.0f, -1.0f, 1.0f); w2 = w2 > 1.0f ? 2.0f - w2 : w2;
                el = w1 * rec_read(rec_l, loop_start, loop_pos) + w2 * rec_read(rec_l, loop_start, p2);
                er = w1 * rec_read(rec_r, loop_start, loop_pos) + w2 * rec_read(rec_r, loop_start, p2);
                loop_pos += 0.5f;
            } else {
                float edge = fx_clamp(loop_pos * (1.0f / 24.0f), 0.0f, 1.0f) *
                             fx_clamp((loop_len - 1.0f - loop_pos) * (1.0f / 24.0f), 0.0f, 1.0f);
                el = edge * rec_read(rec_l, loop_start, loop_pos);
                er = edge * rec_read(rec_r, loop_start, loop_pos);
                if (backwards(fx)) loop_pos -= 0.5f;
                else if (fx == GF_SCRATCH) {        /* back and forth over the slice */
                    loop_pos += 0.5f * scratch_dir;
                    if (loop_pos >= loop_len - 1.0f) { loop_pos = loop_len - 1.0f; scratch_dir = -1.0f; }
                    else if (loop_pos <= 0.0f) { loop_pos = 0.0f; scratch_dir = 1.0f; }
                } else if (fx == GF_OCTAVE) loop_pos += 1.0f; /* twice the speed */
                else {
                    loop_pos += 0.5f * loop_rate;
                    if (fx == GF_PITCHDOWN && loop_rate > 0.25f) loop_rate *= 0.99998f;
                }
            }
            if (loop_pos >= loop_len) {                /* exact length: repeats stay on the beat */
                loop_pos -= loop_len;
                if (fx == GF_SHUFFLE) loop_start = slices_start + (uint32_t)(loop_len * (float)pick(4));
                else if (fx == GF_STUTTER) {
                    static const float sizes[4] = {2.0f, 1.0f, 0.5f, 0.25f}; /* 1/8 .. 1/64 */
                    loop_len = stutter_base * sizes[pick(4)];
                    if (loop_len < 16.0f) loop_len = 16.0f;
                    loop_start = slices_start - (uint32_t)loop_len;
                    loop_pos = 0.0f;
                }
            }
            if (loop_pos < 0.0f) loop_pos += loop_len;
        } else switch (fx) {
        case GF_TAPESTOP: {
            el = rec_read(rec_l, tape_base, tape_frac) * fx_clamp(tape_rate * 3.0f, 0.0f, 1.0f);
            er = rec_read(rec_r, tape_base, tape_frac) * fx_clamp(tape_rate * 3.0f, 0.0f, 1.0f);
            tape_frac += 0.5f * tape_rate;
            if (tape_frac >= 1.0f) { uint32_t w = (uint32_t)tape_frac; tape_base += w; tape_frac -= (float)w; }
            tape_rate = tape_rate > 0.0f ? tape_rate - 1.0f / (0.7f * FX_RATE) : 0.0f;
            break;
        }
        case GF_CRUSH: { /* about 4 bits */
            float q = 6.0f;
            el = (float)(int)(l * q + (l < 0.0f ? -0.5f : 0.5f)) * (1.0f / 6.0f);
            er = (float)(int)(r * q + (r < 0.0f ? -0.5f : 0.5f)) * (1.0f / 6.0f);
            break;
        }
        case GF_DOWNSAMPLE:
            if (++hold_n >= 10U) { hold_n = 0; hold[0] = l; hold[1] = r; }
            el = hold[0]; er = hold[1];
            break;
        case GF_LOWPASS: case GF_HIGHPASS: {
            sweep = sweep < 1.0f ? sweep + 1.0f / (0.8f * FX_RATE) : 1.0f;
            float c = fx_exp2(-sweep * 6.0f);              /* 1 .. 1/64 */
            if (fx == GF_HIGHPASS) c = fx_exp2(-6.0f + sweep * 4.0f); /* rising cut */
            filt[0][0] += (l - filt[0][0]) * c; filt[0][1] += (filt[0][0] - filt[0][1]) * c;
            filt[1][0] += (r - filt[1][0]) * c; filt[1][1] += (filt[1][0] - filt[1][1]) * c;
            el = fx == GF_LOWPASS ? filt[0][1] : l - filt[0][1];
            er = fx == GF_LOWPASS ? filt[1][1] : r - filt[1][1];
            break;
        }
        case GF_GATE:
            gate_phase += gate_step;
            if (gate_phase >= 1.0f) gate_phase -= 1.0f;
            gate_gain += ((gate_phase < 0.5f ? 1.0f : 0.0f) - gate_gain) * 0.05f;
            el = l * gate_gain; er = r * gate_gain;
            break;
        default: /* GF_DROPOUT */
            el = er = 0.0f;
            break;
        }
        left[i] = l + (el - l) * wet;
        right[i] = r + (er - r) * wet;
    }
}

/* ------------------------------------------------------------ render */
FM1_AUDIO_CODE void glitch_render(int32_t *output)
{
    const struct fx_patch *p = &fx_patches[fx_patch_index];
    volatile float *cl = bus_l, *cr = bus_r, *ct = tape_send, *cs = rev_send, *dl = direct_l, *dr = direct_r; /* no Flash memset */
    for (unsigned i = 0; i < BLOCK; ++i) cl[i] = cr[i] = ct[i] = cs[i] = dl[i] = dr[i] = 0.0f;
    /* Auditions from the panel play at once. */
    for (unsigned r = g_auditions.read, w = g_auditions.write; r != w; ++r) {
        FX_BARRIER();
        trigger(g_auditions.slot[r & 15U], 110U, 0);
        g_auditions.read = r + 1U;
    }
    /* Sequencer: find tick boundaries inside this block. */
    float samples_per_tick = 60.0f / fx_clamp(p->bpm, 20.0f, 300.0f) / 16.0f * FX_RATE;
    if (g_run && !was_running) {
        tick = 0; tick_phase = samples_per_tick; event_count = 0;
        for (unsigned s = 0; s < G_SLOTS; ++s) pos[s] = sub[s] = 0;
    }
    was_running = g_run;
    if (g_run) {
        float offset = 0.0f;
        while (offset < (float)BLOCK) {
            float remaining = samples_per_tick - tick_phase;
            if (remaining <= (float)BLOCK - offset) {
                offset += remaining > 0.0f ? remaining : 0.0f;
                on_tick(now + (uint32_t)offset, samples_per_tick);
                tick = (tick + 1U) & 63U;
                tick_phase = 0.0f;
            } else { tick_phase += (float)BLOCK - offset; break; }
        }
    } else {
        event_count = 0;
        if (g_staged_ready) adopt_staged();
    }
    bar_lfo += (float)BLOCK / (samples_per_tick * 64.0f);
    if (bar_lfo >= 1.0f) bar_lfo -= 1.0f;
    /* Due events start voices with their exact offset in this block. */
    for (unsigned i = 0; i < event_count;) {
        int32_t due = (int32_t)(events[i].time - now);
        if (due < BLOCK) {
            trigger(events[i].slot, events[i].velocity, due > 0 ? due : 0);
            events[i] = events[--event_count];
        } else ++i;
    }
    /* Hiss bed: quiet, bright, decorrelated left/right while running. */
    const float hiss = g_run ? g_hiss : 0.0f;
    if (hiss > 0.0f)
        for (unsigned i = 0; i < BLOCK; ++i) {
            float a = bipolar(&hiss_noise), b = bipolar(&hiss_noise);
            hiss_hp[0] += (a - hiss_hp[0]) * 0.2f; hiss_hp[1] += (b - hiss_hp[1]) * 0.2f;
            bus_l[i] += (a - hiss_hp[0]) * hiss;
            bus_r[i] += (b - hiss_hp[1]) * hiss;
        }
    for (unsigned k = 0; k < G_VOICES; ++k) {
        struct voice *v = &voices[k];
        if (!v->active) continue;
        for (unsigned i = 0; i < BLOCK && v->active; ++i) {
            if (v->delay > 0) { --v->delay; continue; }
            float y = voice_sample(v);
            if (v->direct) { direct_l[i] += y * v->gl; direct_r[i] += y * v->gr; continue; }
            bus_l[i] += y * v->gl;
            bus_r[i] += y * v->gr;
            float mono = 0.5f * (v->gl + v->gr) * y;
            tape_send[i] += mono * v->tape;
            rev_send[i] += mono * v->reverb;
        }
    }
    fx_echo(bus_l, bus_r, p, tape_send, 0, 0);
    fx_reverb(bus_l, bus_r, p, rev_send);
    for (unsigned i = 0; i < BLOCK; ++i) { bus_l[i] += direct_l[i]; bus_r[i] += direct_r[i]; }
    /* DROP: hold the dip, then recover over about 15 ms. */
    if (drop_hold > 0) { drop_hold -= BLOCK; drop_level = drop_depth; }
    else drop_level *= 0.75f;
    if (drop_level > 0.001f || drop_lp[0] != 0.0f) {
        float coef = 1.0f - 0.96f * drop_level * drop_dark, gain = 1.0f - 0.9f * drop_level;
        for (unsigned i = 0; i < BLOCK; ++i) {
            drop_lp[0] += (bus_l[i] - drop_lp[0]) * coef; drop_lp[1] += (bus_r[i] - drop_lp[1]) * coef;
            bus_l[i] = drop_lp[0] * gain; bus_r[i] = drop_lp[1] * gain;
        }
        if (drop_level <= 0.001f) drop_lp[0] = drop_lp[1] = 0.0f;
    }
    perform(bus_l, bus_r, samples_per_tick);
    const float level = g_level * 8388607.0f;
    for (unsigned i = 0; i < BLOCK; ++i) {
        output[2 * i] += (int32_t)(soft(bus_l[i]) * level);
        output[2 * i + 1] += (int32_t)(soft(bus_r[i]) * level);
    }
    now += BLOCK;
}
/* Control task: queue an audition (panel key or MIDI). */
void glitch_hit(unsigned slot)
{
    struct g_queue *q = &g_auditions;
    unsigned w = q->write;
    if (slot >= G_SLOTS || w - q->read >= 16U) return;
    q->slot[w & 15U] = (uint8_t)slot;
    FX_BARRIER();
    q->write = w + 1U;
}
