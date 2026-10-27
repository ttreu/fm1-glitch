/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* GLITCH master effects shared by the control task and the audio owner:
 * tape echo, ladder filter, ping-pong delay (fx_echo.c) and the shimmer
 * plate (fx_reverb.c). */
#ifndef FM1_GLITCH_FX_H
#define FM1_GLITCH_FX_H
#include "fm1.h"

#ifdef FM1_HOST_TEST
#define FX_BARRIER() __asm__ volatile("" ::: "memory")
#else
#define FX_BARRIER() __asm__ volatile("csync" ::: "memory")
#endif
#define FX_INLINE static inline __attribute__((always_inline))
#define FX_RATE 44100.0f

/* Derived effect settings, written by control into the inactive slot. */
struct fx_patch {
    /* Ladder filter on the whole bus (after the tape echo). */
    float cutoff, resonance, drive;  /* semitones, feedback 0..~3.9, input gain */
    float lfo_step, lfo_depth;       /* LFO cycles per block, semitones */
    unsigned filter_on;
    float tape_time, tape_feedback, tape_wow, tape_tone, tape_mix; /* time in samples */
    float delay_time, delay_feedback, delay_mix;                     /* time in samples */
    float rev_decay, rev_damp, rev_shimmer, rev_mix, rev_dry;        /* shimmer plate; dry gain */
    float bpm;
};
extern struct fx_patch fx_patches[2];
extern volatile uint8_t fx_patch_index;

/* Tape echo -> ladder filter -> ping-pong delay on the stereo bus, in place.
 * `send` (mono, may be NULL) feeds the tape echo in addition to the bus. */
FM1_AUDIO_CODE void fx_echo(float *left, float *right, const struct fx_patch *p, const float *send,
                            const float *post_l, const float *post_r);
FM1_AUDIO_CODE void fx_reverb(float *left, float *right, const struct fx_patch *p, const float *send);
/* Echo lines store every fourth sample as int16 (lines are powers of two). */
enum { FX_DECIMATE = 4, FX_DELAY_LINE = 8192, FX_TAPE_LINE = 8192 };
#define FX_DELAY_MAX_S ((float)(FX_DELAY_LINE - 64) * FX_DECIMATE / FX_RATE)
#define FX_TAPE_MAX_S ((float)(FX_TAPE_LINE - 512) * FX_DECIMATE / FX_RATE)

/* 2^x without libm; safe for SRAM audio code. The fraction is reduced to
 * -0.5..0.5, where a 6th-order series keeps the relative error below 3e-7. */
FX_INLINE float fx_exp2(float x)
{
    if (x < -60.0f) return 0.0f;
    if (x > 60.0f) x = 60.0f;
    int i = (int)(x < 0.0f ? x - 0.5f : x + 0.5f);
    float f = (x - (float)i) * 0.69314718f;
    float p = 1.0f + f * (1.0f + f * (0.5f + f * (0.16666667f + f * (0.04166667f + f * (0.00833333f + f * 0.00138889f)))));
    union { float f; int32_t i; } u = {p};
    u.i += i * (1 << 23);
    return u.f;
}
FX_INLINE float fx_clamp(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
#endif
