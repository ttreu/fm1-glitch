/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Private GLITCH state shared by the control task and the audio owner. */
#ifndef FM1_GLITCH_INTERNAL_H
#define FM1_GLITCH_INTERNAL_H
#include "glitch.h"
#include "fx.h"

/* The loop the audio plays. Control edits it in place (single writes; a
 * step word may be read half-updated for one tick, which is harmless). */
extern struct g_slot g_live[G_SLOTS];
/* A pattern change: control fills g_staged, then sets g_staged_ready; the
 * audio owner copies it in at the next bar start and clears the flag. */
extern struct g_slot g_staged[G_SLOTS];
extern volatile uint8_t g_staged_ready;
extern volatile uint32_t g_staged_seed;
extern volatile uint32_t g_mutes;            /* bit per slot */
extern volatile uint8_t g_run;
extern volatile uint8_t g_position[G_SLOTS]; /* step each slot plays now */
extern volatile uint32_t g_bars;             /* bars started since power-on */
extern volatile uint32_t g_activity;
extern volatile uint32_t g_seed;             /* scatter positions repeat until it changes */
extern volatile float g_level, g_hiss, g_swing;
extern volatile float g_accent;              /* 0..1: offbeats quieter than downbeats */
extern volatile float g_flam;                /* 0..1: chance of an extra hit between the steps */
extern volatile uint8_t g_perform_now;       /* enum g_perform */
extern volatile uint8_t g_punch_now;         /* enum g_punch */
/* Per-step ratchet of step n: 0 = the slot's RATCHET, 1..3 = X2..X4. */
FX_INLINE unsigned g_step_ratchet(const struct g_slot *d, unsigned n)
{
    return ((unsigned)d->ratchet[(n >> 2) & 15U] >> ((n & 3U) << 1)) & 3U;
}
/* Auditions from control (panel keys, MIDI): one producer, audio consumes. */
struct g_queue { volatile unsigned read, write; uint8_t slot[16]; };
extern struct g_queue g_auditions;
#endif
