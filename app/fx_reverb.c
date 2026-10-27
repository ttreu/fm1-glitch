/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* GLITCH shimmer reverb: a plate in the style of Dattorro's figure-of-
 * eight tank. The shimmer is an octave-up pitch shifter inside the tank's
 * cross-feedback: it replaces part of the recirculating signal instead of
 * adding to it, so the loop gain stays at the decay setting (always stable)
 * while each pass climbs another octave.
 *
 * The plate runs at half rate (22.05 kHz) on int16 lines (about 37 KB);
 * every line wraps by comparison, so no division helpers are needed.
 * Audio owner, SRAM, no libc calls. */
#ifndef FM1_HOST_TEST
#pragma code_seg(".volatile_ram_code")
#pragma const_seg(".volatile_ram.fm1")
#endif
#include "fx.h"

enum { BLOCK = FM1_BLOCK_FRAMES };

/* Dattorro's lengths (29761 Hz) scaled to 22050 Hz. */
enum {
    IN1 = 105, IN2 = 79, IN3 = 281, IN4 = 205,
    MOD_A = 498 + 24, DEL_A = 3300, AP_A = 1334, DEL_B = 2757,
    MOD_C = 673 + 24, DEL_C = 3125, AP_C = 1968, DEL_D = 2344,
    SHIFT = 2048, POOL = IN1 + IN2 + IN3 + IN4 + MOD_A + DEL_A + AP_A + DEL_B +
                         MOD_C + DEL_C + AP_C + DEL_D + SHIFT
};
static int16_t pool[POOL];
struct line { int16_t *buf; int length, at; };
static struct line in1, in2, in3, in4, mod_a, del_a, ap_a, del_b, mod_c, del_c, ap_c, del_d, shift;
static unsigned ready;
static float band, damp_a, damp_c, feed_a, feed_c, lfo, shift_phase, shimmer_lp, last_l, last_r;

FX_INLINE float q(int16_t v) { return (float)v * (1.0f / 8192.0f); }
FX_INLINE int16_t s16(float x)
{
    x = fx_clamp(x * 8192.0f, -32767.0f, 32767.0f);
    return (int16_t)x;
}
FX_INLINE float tap(const struct line *l, int back) /* 1 = newest */
{
    int i = l->at - back;
    if (i < 0) i += l->length;
    return q(l->buf[i]);
}
FX_INLINE float tap_frac(const struct line *l, float back)
{
    int whole = (int)back;
    float frac = back - (float)whole, a = tap(l, whole), b = tap(l, whole + 1);
    return a + (b - a) * frac;
}
FX_INLINE void push(struct line *l, float x)
{
    l->buf[l->at] = s16(x);
    if (++l->at == l->length) l->at = 0;
}
/* Schroeder all-pass on a line of its full length. */
FX_INLINE float allpass(struct line *l, float x, float g)
{
    float d = tap(l, l->length);
    float v = x - g * d;
    push(l, v);
    return d + g * v;
}
FX_INLINE float allpass_mod(struct line *l, float x, float g, float back)
{
    float d = tap_frac(l, back);
    float v = x - g * d;
    push(l, v);
    return d + g * v;
}
FX_INLINE float delay(struct line *l, float x)
{
    float d = tap(l, l->length);
    push(l, x);
    return d;
}
FX_INLINE float soft(float x) /* safety net inside the tank */
{
    x = fx_clamp(x, -1.5f, 1.5f);
    return x - x * x * x * (4.0f / 27.0f);
}
FX_INLINE float sine(float t)
{
    float x = t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t;
    x *= 1.5707963f;
    float x2 = x * x;
    return x * (1.0f - x2 * (0.16666667f - x2 * (0.0083333f - x2 * 0.000198f)));
}
static FM1_AUDIO_CODE void setup(void)
{
    struct line *lines[] = {&in1, &in2, &in3, &in4, &mod_a, &del_a, &ap_a, &del_b, &mod_c, &del_c, &ap_c, &del_d, &shift};
    static const int lengths[] = {IN1, IN2, IN3, IN4, MOD_A, DEL_A, AP_A, DEL_B, MOD_C, DEL_C, AP_C, DEL_D, SHIFT};
    int16_t *at = pool;
    for (unsigned i = 0; i < sizeof lengths / sizeof lengths[0]; ++i) {
        lines[i]->buf = at; lines[i]->length = lengths[i]; lines[i]->at = 0;
        at += lengths[i];
    }
    ready = 1;
}
/* Octave-up shifter: two read heads sweep the line at double speed, each
 * faded by a triangle so the jump back is silent (their gains sum to 1). */
FX_INLINE float octave_up(float x)
{
    push(&shift, x);
    shift_phase += 1.0f / 1024.0f;
    if (shift_phase >= 1.0f) shift_phase -= 1.0f;
    float p2 = shift_phase + 0.5f;
    if (p2 >= 1.0f) p2 -= 1.0f;
    float ga = 1.0f - fx_clamp(2.0f * shift_phase - 1.0f, -1.0f, 1.0f);
    float gb = 1.0f - fx_clamp(2.0f * p2 - 1.0f, -1.0f, 1.0f);
    ga = ga > 1.0f ? 2.0f - ga : ga; /* 1 - |2p - 1| */
    gb = gb > 1.0f ? 2.0f - gb : gb;
    return ga * tap_frac(&shift, 2.0f + (1.0f - shift_phase) * 1024.0f) +
           gb * tap_frac(&shift, 2.0f + (1.0f - p2) * 1024.0f);
}
/* One plate sample at 22.05 kHz. */
FX_INLINE void plate(float in, const struct fx_patch *p, float *out_l, float *out_r)
{
    const float decay = p->rev_decay, damp = p->rev_damp;
    /* Shimmer: crossfade part of the recirculation to an octave-up copy. */
    const float s = p->rev_shimmer;
    float up = 0.0f;
    if (s > 0.0f) {
        /* x1.2: the two crossfaded read heads lose about 1.6 dB on average. */
        shimmer_lp += (1.2f * octave_up(0.5f * (feed_a + feed_c)) - shimmer_lp) * 0.6f;
        up = shimmer_lp;
    }
    float back_a = feed_c + s * (up - feed_c), back_c = feed_a + s * (up - feed_a);
    band += (in - band) * 0.7f;
    float x = allpass(&in1, band, 0.75f);
    x = allpass(&in2, x, 0.75f);
    x = allpass(&in3, x, 0.625f);
    x = allpass(&in4, x, 0.625f);
    lfo += 1.0f / 22050.0f;
    if (lfo >= 1.0f) lfo -= 1.0f;
    float m = 12.0f * sine(lfo);
    /* Left half of the figure eight. */
    float a = allpass_mod(&mod_a, soft(x + back_a * decay), -0.7f, (float)(MOD_A - 24) + m);
    a = delay(&del_a, a);
    damp_a += (a - damp_a) * damp;
    a = allpass(&ap_a, damp_a * decay, 0.5f);
    feed_a = delay(&del_b, a);
    /* Right half. */
    float c = allpass_mod(&mod_c, soft(x + back_c * decay), -0.7f, (float)(MOD_C - 24) - m);
    c = delay(&del_c, c);
    damp_c += (c - damp_c) * damp;
    c = allpass(&ap_c, damp_c * decay, 0.5f);
    feed_c = delay(&del_d, c);
    /* Output taps (scaled from Dattorro's table). */
    *out_l = 0.5f * (tap(&del_c, 197) + tap(&del_c, 2204) - tap(&ap_c, 1418) + tap(&del_d, 1479) -
                     tap(&del_a, 1475) - tap(&ap_a, 139) - tap(&del_b, 790));
    *out_r = 0.5f * (tap(&del_a, 262) + tap(&del_a, 2952) - tap(&ap_a, 909) + tap(&del_b, 1993) -
                     tap(&del_c, 1641) - tap(&ap_c, 139) - tap(&del_d, 267));
    if (!(feed_a > -4.0f && feed_a < 4.0f) || !(feed_c > -4.0f && feed_c < 4.0f)) feed_a = feed_c = 0.0f;
}
FM1_AUDIO_CODE void fx_reverb(float *left, float *right, const struct fx_patch *p, const float *send)
{
    if (!ready) setup();
    if (p->rev_mix <= 0.0f) return;
    const float mix = p->rev_mix, dry = p->rev_dry;
    /* Half rate: each sample pair feeds the plate once; the output is
     * interpolated back to 44.1 kHz. BLOCK is even, so pairs never split. */
    for (unsigned i = 0; i < BLOCK; i += 2) {
        float in = 0.25f * (left[i] + right[i] + left[i + 1] + right[i + 1]);
        if (send) in += 0.5f * (send[i] + send[i + 1]);
        float l, r;
        plate(in, p, &l, &r);
        left[i] = left[i] * dry + 0.5f * (last_l + l) * mix;
        right[i] = right[i] * dry + 0.5f * (last_r + r) * mix;
        left[i + 1] = left[i + 1] * dry + l * mix;
        right[i + 1] = right[i + 1] * dry + r * mix;
        last_l = l; last_r = r;
    }
}
