/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* GLITCH: the FM-1 as a microsound drum machine.
 *
 * 27 slots, one per key. A is a classic hi-hat; every other slot plays a
 * synthesized, non-tonal microsound (clicks, click trains, digital crunch,
 * noise ticks, chirps, zaps, noise snaps, dust, bit noise, grains, micro
 * kicks, pops, too-short loops, very high beeps, dropouts).
 * Each slot has up to 64 steps with its own loop length (polymeter), rate,
 * probability, ratchets, micro-timing, scatter, panning and effect sends.
 * Loops can mutate on their own from bar to bar ("living loops"), a random
 * button rolls a whole new beat, 16 patterns are stored in Flash, and the
 * master bus has tape echo, filter, delay, shimmer reverb and momentary
 * stutter / repeat effects. */
#ifndef FM1_GLITCH_H
#define FM1_GLITCH_H
#include "fm1.h"

#define FM1_GLITCH 1
enum { G_SLOTS = 27, G_STEPS = 64, G_VOICES = 12, G_PATTERNS = 16, G_PAGE_STEPS = 16 };
/* Physical key of the one classic sound: A (third white key). */
enum { G_KEY_HAT = 4 };

enum g_source {
    GS_HAT,      /* classic hi-hat (slot A only) */
    GS_IMPULSE,  /* a single 0.1-1.5 ms digital click */
    GS_TRAIN,    /* 3-20 impulses, 2-12 ms apart: a short rasp */
    GS_CRUNCH,   /* sample-and-hold digital noise: a crunchy burst */
    GS_TICK,     /* a 2-20 ms burst of noise */
    GS_CHIRP,    /* a fast sine sweep up or down */
    GS_ZAP,      /* a falling laser-like sweep */
    GS_SNAP,     /* a dry, gated band of noise with a click: a snap */
    GS_DUST,     /* a crackle of sparse random clicks */
    GS_BITS,     /* 8-bit style LFSR noise */
    GS_GRAIN,    /* a resonant noise grain */
    GS_THUMP,    /* a micro kick: one to a few cycles of a low, falling sine */
    GS_POP,      /* a speaker pop: a jump that slides back to zero */
    GS_LOOP,     /* a few ms of noise looped too short: a buzzing stutter */
    GS_HIGH,     /* a very high, short sine beep (4-18 kHz) */
    GS_DROP,     /* silent: a dropout of the whole bus, effects included */
    GS_COUNT
};
enum g_param {
    GP_SOURCE, GP_PITCH, GP_DECAY, GP_COLOR,     /* SOUND  */
    GP_PROB, GP_RATCHET, GP_MICRO, GP_SCATTER,   /* RHYTHM */
    GP_LENGTH, GP_RATE, GP_RANDOM, GP_LEVEL,     /* LOOP   */
    GP_PAN_MODE, GP_PAN, GP_TAPE, GP_SHIMMER,    /* SPACE  */
    GP_REVERSE,                                  /* REVERSE: chance (%) a hit plays backwards */
    GP_COUNT
};
/* PAN_JUMP: in the middle, but now and then (PAN = how often) a hit jumps
 * all the way to one side, dry: 100 % there, 0 % on the other side. */
enum { PAN_FIXED, PAN_RANDOM, PAN_PINGPONG, PAN_LFO, PAN_JUMP };
struct g_slot {
    uint64_t steps;            /* bit n: step n plays (n < LENGTH) */
    uint8_t value[GP_COUNT];
    /* Per-step ratchet, 2 bits per step (step n: byte n/4, bits 2*(n%4)):
     * 0 = the slot's RATCHET, 1..3 = X2..X4 on this step only. */
    uint8_t ratchet[G_STEPS / 4];
};
/* Settings of the whole pattern (stored with it). */
enum g_global {
    GG_BPM, GG_SWING, GG_MUTATE, GG_LEVEL,                    /* HOME  */
    GG_ACCENT, GG_FLAM, GG_HISS, GG_DENSITY,                  /* KIT */
    GG_FILTER, GG_RESO, GG_LFO_RATE, GG_LFO_DEPTH,            /* FX 1  */
    GG_TAPE_TIME, GG_TAPE_FEEDBACK, GG_TAPE_WOW, GG_TAPE_MIX, /* FX 2  */
    GG_DELAY_TIME, GG_DELAY_FEEDBACK, GG_DELAY_MIX, GG_DRIVE, /* FX 3  */
    GG_REV_SIZE, GG_REV_DAMP, GG_SHIMMER, GG_REV_MIX,         /* FX 4  */
    GG_TAPE_SYNC, GG_DELAY_SYNC, GG_LFO_SYNC, GG_TAPE_TONE,   /* FX 5  */
    GG_COUNT
};
/* Momentary master effects: hold FX + white key 1..16; the last five come
 * from black keys. */
enum g_perform {
    GF_OFF, GF_REPEAT_4, GF_REPEAT_8, GF_REPEAT_16, GF_REPEAT_32, GF_REPEAT_64, GF_REVERSE_4, GF_REVERSE_8,
    GF_PITCHDOWN, GF_TAPESTOP, GF_FREEZE, GF_CRUSH, GF_DOWNSAMPLE, GF_LOWPASS, GF_HIGHPASS, GF_GATE,
    GF_DROPOUT,
    GF_REVERSE_16,  /* the last 1/16 backwards, looped */
    GF_SCRATCH,     /* the last 1/8 back and forth */
    GF_SHUFFLE,     /* random 1/16 slices of the last beat */
    GF_OCTAVE,      /* the last 1/16 an octave up, looped */
    GF_STUTTER,     /* repeats whose length jumps between 1/8 and 1/64 */
    GF_COUNT
};
/* Momentary sequencer effects (punch-in): hold FX + black key. They change
 * the hits the sequencer plays and can run together with a master effect. */
enum g_punch {
    GX_OFF,
    GX_RATCHET_2, GX_RATCHET_3, GX_RATCHET_4, /* every hit as 2, 3 or 4 */
    GX_ROLL,        /* every hit as an accelerating, rising roll of 5 */
    GX_REVERSE,     /* every hit backwards */
    GX_CHAOS,       /* every hit with a random pitch, half of them backwards */
    GX_COUNT
};

/* ---- control task */
void glitch_init(void);
void glitch_poll(uint32_t now_ms);                    /* mutation, MIDI, tempo, pattern changes */
/* Slots */
unsigned glitch_slot_get(unsigned slot, unsigned param);
void glitch_slot_edit(unsigned slot, unsigned param, int delta);
unsigned glitch_slot_max(unsigned param);
const char *glitch_slot_param_name(unsigned param);
void glitch_slot_text(unsigned slot, unsigned param, char out[12]);
void glitch_slot_name(unsigned slot, char out[12]);  /* its source */
uint64_t glitch_steps(unsigned slot);
/* SEQ + key: off -> on -> X2 -> X3 -> X4 -> off. Returns the new state:
 * 0 off, 1 on, 2..4 on with a ratchet of 2..4 hits. */
unsigned glitch_toggle_step(unsigned slot, unsigned step);
unsigned glitch_step_ratchet(unsigned slot, unsigned step); /* 0, or 1..3 = X2..X4 */
unsigned glitch_position(unsigned slot);              /* step playing now */
void glitch_hit(unsigned slot);                       /* audition */
void glitch_randomize_slot(unsigned slot, uint32_t seed);
/* Mutes */
uint32_t glitch_mutes(void);
void glitch_toggle_mute(unsigned slot);
/* Pattern */
void glitch_transport(int run);                       /* 1 start, 0 stop, -1 toggle */
unsigned glitch_running(void);
void glitch_randomize(uint32_t seed);                 /* a completely new beat */
void glitch_reset_loop(void);                         /* undo the mutations */
unsigned glitch_mutated(void);                        /* live loop differs from the pattern */
unsigned glitch_pattern(void);                        /* 0..15 */
int glitch_pattern_next(void);                        /* queued pattern or -1 */
void glitch_select_pattern(unsigned pattern);         /* at the next bar while running */
int glitch_save(unsigned pattern);                    /* store the live loop; 0 = ok */
unsigned glitch_pattern_stored(unsigned pattern);     /* 1 if Flash holds a user pattern */
/* Global settings */
unsigned glitch_global_get(unsigned param);
void glitch_global_edit(unsigned param, int delta);
const char *glitch_global_name(unsigned param);
void glitch_global_text(unsigned param, char out[12]);
/* Tempo: BPM = 40 + value. */
unsigned glitch_bpm(void);
unsigned glitch_bpm_external(void);
void glitch_tap(uint32_t now_ms);
/* MIDI input context: clock / start / stop bytes and channel messages
 * (notes from C1 play the slots, from any channel). */
void glitch_midi_realtime(uint8_t status, uint32_t now_ms);
void glitch_midi_message(const uint8_t *message, size_t bytes);
/* Momentary master effect (GF_OFF ends it). */
void glitch_perform(unsigned effect);
unsigned glitch_performing(void);
const char *glitch_perform_name(unsigned effect);
void glitch_punch(unsigned effect);                   /* enum g_punch; GX_OFF ends it */
unsigned glitch_punching(void);
const char *glitch_punch_name(unsigned effect);
uint32_t glitch_activity(void);                       /* bit per slot: hit in the last ~100 ms */

/* ---- audio owner */
FM1_AUDIO_CODE void glitch_render(int32_t *output);
#endif
