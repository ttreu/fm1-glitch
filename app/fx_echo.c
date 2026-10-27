/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* GLITCH master effects, in this order: a tape echo with wow/flutter and
 * saturating feedback (it self-oscillates above 100%), a 24 dB ladder filter
 * with drive and LFO (so it can sweep the echo too), and a ping-pong stereo
 * delay. Audio owner, SRAM, no libc calls.
 *
 * Echo lines store every fourth sample as int16 (about 5.5 kHz bandwidth,
 * like a tape or bucket-brigade echo). Line
 * sizes are powers of two: indexing uses masks, never division helpers. */
#ifndef FM1_HOST_TEST
#pragma code_seg(".volatile_ram_code")
#pragma const_seg(".volatile_ram.fm1")
#endif
#include "fx.h"

enum { BLOCK = FM1_BLOCK_FRAMES, D = FX_DECIMATE };

struct line {
    uint32_t write;
    unsigned phase;
    float sum;
};
static int16_t delay_l[FX_DELAY_LINE], delay_r[FX_DELAY_LINE], tape_buf[FX_TAPE_LINE];
static struct line dl_line, dr_line, tape_line;
static float ladder[2][4], lfo, wow, flutter, tape_delay, delay_delay;
static float tape_lp, tape_hp;
static uint32_t noise = 22222U;

FX_INLINE float soft(float x)
{
    x = fx_clamp(x, -1.5f, 1.5f);
    return x - x * x * x * (4.0f / 27.0f);
}
FX_INLINE float sine(float t) /* sin(2 pi t), t in [0,1) */
{
    float x = t < 0.5f ? 4.0f * t - 1.0f : 3.0f - 4.0f * t;
    x *= 1.5707963f;
    float x2 = x * x;
    return x * (1.0f - x2 * (0.16666667f - x2 * (0.0083333f - x2 * 0.000198f)));
}
/* Average D input samples into one stored sample (a gentle anti-alias). */
FX_INLINE void line_write(int16_t *buf, uint32_t mask, struct line *l, float x)
{
    l->sum += x;
    if (++l->phase < D) return;
    float y = fx_clamp(l->sum * (16384.0f / D), -32767.0f, 32767.0f);
    buf[l->write & mask] = (int16_t)y;
    l->write++;
    l->phase = 0;
    l->sum = 0.0f;
}
/* Read `samples` (full-rate, fractional) behind the write position. */
FX_INLINE float line_read(const int16_t *buf, uint32_t mask, const struct line *l, float samples)
{
    float back = (samples - (float)l->phase) * (1.0f / D) + 1.0f;
    int whole = (int)back;
    float frac = back - (float)whole;
    uint32_t i = l->write - (uint32_t)whole;
    float a = buf[i & mask], b = buf[(i - 1U) & mask];
    return (a + (b - a) * frac) * (1.0f / 16384.0f);
}
FM1_AUDIO_CODE void fx_echo(float *left, float *right, const struct fx_patch *p, const float *send,
                                  const float *post_l, const float *post_r)
{
    const float tape_target = fx_clamp(p->tape_time, 32.0f, FX_TAPE_MAX_S * FX_RATE);
    const float delay_target = fx_clamp(p->delay_time, 32.0f, FX_DELAY_MAX_S * FX_RATE);
    const uint32_t tmask = FX_TAPE_LINE - 1U, dmask = FX_DELAY_LINE - 1U;
    noise = noise * 1664525U + 1013904223U;
    const float jitter = (float)(int32_t)noise * (1.0f / 2147483648.0f);
    /* ---- 1. tape echo: wow (0.6 Hz) + flutter (5.7 Hz) + a little drift */
    for (unsigned i = 0; i < BLOCK; ++i) {
        /* Time changes slew slowly, bending pitch like a tape speed change. */
        tape_delay += (tape_target - tape_delay) * 0.0004f;
        wow += 0.6f / FX_RATE; if (wow >= 1.0f) wow -= 1.0f;
        flutter += 5.7f / FX_RATE; if (flutter >= 1.0f) flutter -= 1.0f;
        float mod = p->tape_wow * (0.006f * sine(wow) + 0.0012f * sine(flutter) + 0.0006f * jitter);
        float dry = 0.5f * (left[i] + right[i]) + (send ? send[i] : 0.0f);
        float echo = line_read(tape_buf, tmask, &tape_line, tape_delay * (1.0f + mod));
        /* Feedback: tone low-pass, DC/bass trim, then tape saturation. */
        tape_lp += (echo - tape_lp) * p->tape_tone;
        tape_hp += (tape_lp - tape_hp) * 0.004f;
        float wet = tape_lp - tape_hp;
        line_write(tape_buf, tmask, &tape_line, soft(dry + soft(wet * p->tape_feedback)));
        left[i] += wet * p->tape_mix;
        right[i] += wet * p->tape_mix;
    }
    /* ---- 2. ladder filter on voices + tape, so it can sweep a singing tape */
    lfo += p->lfo_step;
    if (lfo >= 1.0f) lfo -= 1.0f;
    if (p->filter_on) {
        float semis = p->cutoff + p->lfo_depth * sine(lfo);
        float hz = fx_clamp(440.0f * fx_exp2((semis - 69.0f) * (1.0f / 12.0f)), 20.0f, 16000.0f);
        float g = 1.0f - fx_exp2(-6.2831853f * 1.4426950f / FX_RATE * hz);
        const float k = p->resonance, drive = p->drive;
        const float out_gain = (1.0f + 0.5f * k) / (0.6f + 0.4f * drive);
        float *bus[2] = {left, right};
        for (unsigned c = 0; c < 2; ++c) {
            float *s = ladder[c], *x = bus[c];
            float y1 = s[0], y2 = s[1], y3 = s[2], y4 = s[3];
            for (unsigned i = 0; i < BLOCK; ++i) {
                float u = soft(x[i] * drive - k * y4);
                y1 += g * (u - y1); y2 += g * (y1 - y2); y3 += g * (y2 - y3); y4 += g * (y3 - y4);
                x[i] = y4 * out_gain;
            }
            if (!(y4 > -10.0f && y4 < 10.0f)) y1 = y2 = y3 = y4 = 0.0f;
            s[0] = y1; s[1] = y2; s[2] = y3; s[3] = y4;
        }
    }
    /* Optional extra signal past the filter, into the delay and reverb. */
    if (post_l && post_r)
        for (unsigned i = 0; i < BLOCK; ++i) { left[i] += post_l[i]; right[i] += post_r[i]; }
    /* ---- 3. ping-pong delay */
    for (unsigned i = 0; i < BLOCK; ++i) {
        delay_delay += (delay_target - delay_delay) * 0.0004f;
        float in = 0.5f * (left[i] + right[i]);
        float a = line_read(delay_l, dmask, &dl_line, delay_delay);
        float b = line_read(delay_r, dmask, &dr_line, delay_delay);
        line_write(delay_l, dmask, &dl_line, soft(in + b * p->delay_feedback));
        line_write(delay_r, dmask, &dr_line, soft(a * p->delay_feedback));
        left[i] += a * p->delay_mix;
        right[i] += b * p->delay_mix;
    }
}
