/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* Modified 2026-10 by Thomas Treu for GLITCH: one silent voice instead of the DX7 factory bank under FM1_GLITCH. */
#include "fm_patch.h"
#include "glitch.h"

#if FM1_GLITCH
/* GLITCH plays no DX7: one silent voice instead of the 128-voice factory
 * bank (10.5 KB of program Flash and 16 KB of SRAM). */
static FM1_DSP_DATA const uint8_t silent_voice[128];
FM1_DSP_CODE const uint8_t *groove_fm_factory_raw(unsigned program)
{
    (void)program;
    return silent_voice;
}
#else
#include "fm_factory.inc"

/* Expand once during startup, before the audio owners are running. MIDI
 * program changes then read SRAM without decompression or heap allocation. */
static uint8_t factory_raw[FM1_FM_PROGRAMS*128];
static unsigned factory_ready;
static FM1_DSP_CODE void factory_init(void)
{
    if (factory_ready) return;
    uint8_t *out=factory_raw;
    for (unsigned at=0;at<sizeof factory_packed;) {
        unsigned token=factory_packed[at++];
        if (token<128) *out++=(uint8_t)token;
        else {
            unsigned low=factory_packed[at++],distance=low&127U;
            if (low&128U) distance|=(unsigned)factory_packed[at++]<<7;
            for (unsigned n=token-128+3;n;--n) { *out=out[-(int)distance]; ++out; }
        }
    }
    factory_ready=1;
}
FM1_DSP_CODE const uint8_t *groove_fm_factory_raw(unsigned program)
{
    factory_init();
    return factory_raw+(program%FM1_FM_PROGRAMS)*128;
}
#endif
FM1_DSP_CODE void groove_fm_factory(uint8_t p[156], unsigned program)
{
    groove_fm_unpack(p,groove_fm_factory_raw(program));
}
