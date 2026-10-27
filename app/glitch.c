/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* GLITCH control side: parameters, the factory patterns, random beats,
 * living loops (mutation), the 16 pattern memories in Flash, tempo and MIDI.
 * Control task only. */
#include "glitch_internal.h"
#include "instrument.h"
#include <string.h>

/* ------------------------------------------------------------ parameters */
static const struct { char name[10]; uint8_t min, max; } slot_params[GP_COUNT] = {
    {"SOURCE", GS_IMPULSE, GS_COUNT - 1}, {"PITCH", 0, 48}, {"DECAY", 0, 99}, {"COLOR", 0, 99},
    {"PROB", 0, 100}, {"RATCHET", 0, 4}, {"MICRO", 0, 99}, {"SCATTER", 0, 32},
    {"LENGTH", 1, G_STEPS}, {"RATE", 0, 2}, {"RANDOM", 0, 99}, {"LEVEL", 0, 99},
    {"PAN MODE", 0, 4}, {"PAN", 0, 99}, {"TAPE", 0, 99}, {"SHIMMER", 0, 99},
    {"REVERSE", 0, 100}};
static const char source_names[GS_COUNT][8] = {
    "HIHAT", "IMPULSE", "TRAIN", "CRUNCH", "TICK", "CHIRP", "ZAP", "SNAP", "DUST", "BITS", "GRAIN", "THUMP", "POP", "LOOP", "HIGH", "DROP"};
static const struct { char name[10]; uint8_t max; } global_params[GG_COUNT] = {
    {"BPM", 200}, {"SWING", 99}, {"MUTATE", 99}, {"LEVEL", 99},
    {"ACCENT", 99}, {"FLAM", 99}, {"HISS", 99}, {"DENSITY", 99},
    {"CUTOFF", 99}, {"RESO", 99}, {"LFO RATE", 99}, {"LFO AMT", 99},
    {"TAPE TM", 99}, {"TAPE FB", 120}, {"WOW", 99}, {"TAPE MIX", 99},
    {"DLY TIME", 99}, {"DLY FB", 99}, {"DLY MIX", 99}, {"DRIVE", 99},
    {"SIZE", 99}, {"DAMP", 99}, {"SHIMMER", 99}, {"REV MIX", 99},
    {"TAPE SYNC", 1}, {"DLY SYNC", 1}, {"LFO SYNC", 1}, {"TAPE TONE", 99}};
static const char perform_names[GF_COUNT][12] = {
    "", "REPEAT 1/4", "REPEAT 1/8", "REPEAT 1/16", "REPEAT 1/32", "REPEAT 1/64", "REVERSE 1/4", "REVERSE 1/8",
    "PITCH DOWN", "TAPE STOP", "FREEZE", "BITCRUSH", "DOWNSAMPLE", "LOWPASS", "HIGHPASS", "GATE 1/32", "DROPOUT",
    "REVERSE 1/16", "SCRATCH", "SHUFFLE", "OCTAVE UP", "STUTTER"};
const char *glitch_perform_name(unsigned fx) { return fx < GF_COUNT ? perform_names[fx] : ""; }
static const char punch_names[GX_COUNT][12] = {"", "RATCHET X2", "RATCHET X3", "RATCHET X4", "ROLL", "REVERSE ALL", "CHAOS"};
const char *glitch_punch_name(unsigned fx) { return fx < GX_COUNT ? punch_names[fx] : ""; }
/* Tempo-synced note values, in beats (quarter notes). */
static const struct { char name[6]; float beats; } echo_notes[] = {
    {"1/16", 0.25f}, {"1/8T", 1.0f / 3.0f}, {"1/16D", 0.375f}, {"1/8", 0.5f}, {"1/4T", 2.0f / 3.0f},
    {"1/8D", 0.75f}, {"1/4", 1.0f}, {"1/2T", 4.0f / 3.0f}, {"1/4D", 1.5f}, {"1/2", 2.0f}, {"1/1", 4.0f}};
static const struct { char name[7]; float beats; } lfo_notes[] = {
    {"8 BARS", 32.0f}, {"4 BARS", 16.0f}, {"2 BARS", 8.0f}, {"1 BAR", 4.0f},
    {"1/2", 2.0f}, {"1/4", 1.0f}, {"1/8", 0.5f}, {"1/16", 0.25f}};
enum { ECHO_NOTES = sizeof echo_notes / sizeof echo_notes[0], LFO_NOTES = sizeof lfo_notes / sizeof lfo_notes[0] };
static unsigned note_index(unsigned v, unsigned count) { unsigned i = v * count / 100U; return i < count ? i : count - 1U; }
static float unit(unsigned v) { return (float)v / 99.0f; }

/* ------------------------------------------------------------ pattern state */
/* One stored pattern: everything but the transport. One Flash sector each. */
struct g_record {
    uint32_t magic, version, crc;
    uint32_t mutes, seed;
    uint8_t global[GG_COUNT];
    struct g_slot slot[G_SLOTS];
};
enum { RECORD_MAGIC = 0x31544c47U /* "GLT1" */, RECORD_VERSION = 4, /* 2: step ratchets, REVERSE; 3: THUMP, POP, ACCENT, FLAM; 4: LOOP, HIGH */
       PATTERN_BASE = GROOVE_LIBRARY_OFFSET - G_PATTERNS * FM1_FLASH_SECTOR_BYTES };
_Static_assert(sizeof(struct g_record) <= FM1_FLASH_SECTOR_BYTES, "pattern fits a sector");
_Static_assert(PATTERN_BASE >= GROOVE_SAMPLE_OFFSET, "pattern area inside the user partition");
static struct g_record home;        /* the pattern as loaded / saved: mutation returns here */
static unsigned pattern_now, pending_globals;
static int pattern_next = -1;
static uint32_t bars_seen, activity_seen;

static void put_number(char *out, int v)
{
    char tmp[8]; unsigned n = 0, u = (unsigned)(v < 0 ? -v : v);
    if (v < 0) *out++ = '-';
    do { tmp[n++] = (char)('0' + u % 10U); u /= 10U; } while (u && n < 7);
    while (n) *out++ = tmp[--n];
    *out = 0;
}
static void seconds(char *out, float s)
{
    if (s < 1.0f) { put_number(out, (int)(s * 1000.0f + 0.5f)); strcat(out, "MS"); return; }
    unsigned tenths = (unsigned)(s * 10.0f + 0.5f);
    put_number(out, (int)(tenths / 10U)); strcat(out, "."); put_number(out + strlen(out), (int)(tenths % 10U)); strcat(out, "S");
}
static unsigned fixed_slot(unsigned slot) { return slot == G_KEY_HAT; }

/* ------------------------------------------------------------ derived settings */
static float beat_s(void) { return 60.0f / (float)(40U + home.global[GG_BPM]); }
static float tape_time_s(void)
{
    const uint8_t *g = home.global;
    if (g[GG_TAPE_SYNC]) return echo_notes[note_index(g[GG_TAPE_TIME], ECHO_NOTES)].beats * beat_s();
    float u = unit(g[GG_TAPE_TIME]); return 0.04f + u * u * (FX_TAPE_MAX_S - 0.04f);
}
static float delay_time_s(void)
{
    const uint8_t *g = home.global;
    if (g[GG_DELAY_SYNC]) return echo_notes[note_index(g[GG_DELAY_TIME], ECHO_NOTES)].beats * beat_s();
    float u = unit(g[GG_DELAY_TIME]); return 0.02f + u * u * (FX_DELAY_MAX_S - 0.02f);
}
static float lfo_rate_hz(void)
{
    const uint8_t *g = home.global;
    if (g[GG_LFO_SYNC]) return 1.0f / (lfo_notes[note_index(g[GG_LFO_RATE], LFO_NOTES)].beats * beat_s());
    return 0.03f * fx_exp2(unit(g[GG_LFO_RATE]) * 7.6f);
}
static float one_pole(float hz) { return 1.0f - fx_exp2(-6.2831853f * 1.4426950f / FX_RATE * hz); }
static void derive(void)
{
    struct fx_patch *p = &fx_patches[fx_patch_index ^ 1U];
    const uint8_t *g = home.global;
    p->filter_on = g[GG_FILTER] < 99 || g[GG_RESO] || g[GG_DRIVE];
    p->cutoff = 20.0f + (float)g[GG_FILTER] * 1.17f;
    p->resonance = unit(g[GG_RESO]) * 3.9f;
    p->drive = 1.0f + unit(g[GG_DRIVE]) * 4.0f;
    p->lfo_step = lfo_rate_hz() * FM1_BLOCK_FRAMES / FX_RATE;
    p->lfo_depth = unit(g[GG_LFO_DEPTH]) * 36.0f;
    p->tape_time = fx_clamp(tape_time_s(), 0.01f, FX_TAPE_MAX_S) * FX_RATE;
    p->tape_feedback = (float)g[GG_TAPE_FEEDBACK] * 0.01f;
    p->tape_wow = unit(g[GG_TAPE_WOW]);
    p->tape_tone = one_pole(600.0f * fx_exp2(unit(g[GG_TAPE_TONE]) * 3.5f));
    p->tape_mix = unit(g[GG_TAPE_MIX]) * 0.9f;
    p->delay_time = fx_clamp(delay_time_s(), 0.01f, FX_DELAY_MAX_S) * FX_RATE;
    p->delay_feedback = unit(g[GG_DELAY_FEEDBACK]) * 0.92f;
    p->delay_mix = unit(g[GG_DELAY_MIX]) * 0.8f;
    p->rev_decay = 0.25f + unit(g[GG_REV_SIZE]) * 0.70f;
    p->rev_damp = one_pole(1200.0f * fx_exp2((1.0f - unit(g[GG_REV_DAMP])) * 3.5f) * 0.5f);
    p->rev_shimmer = unit(g[GG_SHIMMER]) * 0.95f;
    p->rev_mix = unit(g[GG_REV_MIX]) * 1.6f;
    p->rev_dry = 1.0f - 0.9f * fx_clamp(unit(g[GG_REV_MIX]) - 0.5f, 0.0f, 0.5f);
    p->bpm = (float)(40U + g[GG_BPM]);
    FX_BARRIER();
    fx_patch_index ^= 1U;
    g_level = unit(g[GG_LEVEL]) * unit(g[GG_LEVEL]) * 0.9f;
    g_hiss = (float)g[GG_HISS] * (float)g[GG_HISS] * (0.02f / (99.0f * 99.0f));
    g_swing = unit(g[GG_SWING]) * 0.5f; /* up to half a sixteenth late */
    g_accent = unit(g[GG_ACCENT]) * 0.8f;
    g_flam = unit(g[GG_FLAM]);
}

/* ------------------------------------------------------------ slots */
unsigned glitch_slot_max(unsigned p) { return p < GP_COUNT ? slot_params[p].max : 0; }
const char *glitch_slot_param_name(unsigned p) { return p < GP_COUNT ? slot_params[p].name : ""; }
unsigned glitch_slot_get(unsigned slot, unsigned p) { return slot < G_SLOTS && p < GP_COUNT ? g_live[slot].value[p] : 0; }
uint64_t glitch_steps(unsigned slot) { return slot < G_SLOTS ? g_live[slot].steps : 0; }
unsigned glitch_position(unsigned slot) { return slot < G_SLOTS ? g_position[slot] : 0; }
void glitch_slot_name(unsigned slot, char out[12])
{
    unsigned s = glitch_slot_get(slot, GP_SOURCE);
    strcpy(out, source_names[s < GS_COUNT ? s : 0]);
}
void glitch_slot_text(unsigned slot, unsigned p, char out[12])
{
    static const char ratchets[5][5] = {"OFF", "X2", "X3", "X4", "ROLL"};
    static const char pans[5][10] = {"FIXED", "RANDOM", "PINGPONG", "LFO", "JUMP"};
    unsigned v = glitch_slot_get(slot, p);
    switch (p) {
    case GP_SOURCE: glitch_slot_name(slot, out); return;
    case GP_PITCH: put_number(out, (int)v - 24); return;
    case GP_PROB: put_number(out, (int)v); strcat(out, "%"); return;
    case GP_RATCHET: strcpy(out, ratchets[v < 5 ? v : 0]); return;
    case GP_MICRO: put_number(out, (int)(v * 10U / 99U)); strcat(out, "MS"); return;
    case GP_SCATTER: if (!v) { strcpy(out, "OFF"); return; } put_number(out, (int)v); strcat(out, "/BAR"); return;
    case GP_REVERSE: if (!v) { strcpy(out, "OFF"); return; } put_number(out, (int)v); strcat(out, "%"); return;
    case GP_PAN_MODE: strcpy(out, pans[v < 5 ? v : 0]); return;
    case GP_RATE: strcpy(out, v == 0 ? "1/16" : v == 1 ? "1/32" : "1/64"); return;
    case GP_PAN:
        if (glitch_slot_get(slot, GP_PAN_MODE) == PAN_FIXED) {
            int pan = (int)v * 2 - 99;
            if (pan > -3 && pan < 3) { strcpy(out, "C"); return; }
            out[0] = pan < 0 ? 'L' : 'R'; put_number(out + 1, (pan < 0 ? -pan : pan) / 2); return;
        }
        put_number(out, (int)v); strcat(out, "%"); return;
    default: put_number(out, (int)v); return;
    }
}
void glitch_slot_edit(unsigned slot, unsigned p, int delta)
{
    if (slot >= G_SLOTS || p >= GP_COUNT || !delta) return;
    if (p == GP_SOURCE && fixed_slot(slot)) return; /* the hi-hat on A stays classic */
    int v = (int)g_live[slot].value[p] + delta, lo = slot_params[p].min, hi = slot_params[p].max;
    v = v < lo ? lo : v > hi ? hi : v;
    g_live[slot].value[p] = home.slot[slot].value[p] = (uint8_t)v;
}
static void ratchet_set(struct g_slot *d, unsigned n, unsigned r)
{
    unsigned shift = (n & 3U) * 2U;
    d->ratchet[n >> 2] = (uint8_t)((d->ratchet[n >> 2] & ~(3U << shift)) | ((r & 3U) << shift));
}
unsigned glitch_step_ratchet(unsigned slot, unsigned s)
{
    return slot < G_SLOTS && s < G_STEPS ? g_step_ratchet(&g_live[slot], s) : 0;
}
unsigned glitch_toggle_step(unsigned slot, unsigned s)
{
    if (slot >= G_SLOTS || s >= G_STEPS) return 0;
    struct g_slot *d = &g_live[slot];
    uint64_t bit = (uint64_t)1 << s;
    unsigned r = g_step_ratchet(d, s), state;
    if (!(d->steps & bit)) { ratchet_set(d, s, 0); d->steps |= bit; state = 1; }
    else if (r < 3U) { ratchet_set(d, s, r + 1U); state = r + 2U; }
    else { d->steps &= ~bit; ratchet_set(d, s, 0); state = 0; }
    home.slot[slot].steps = (home.slot[slot].steps & ~bit) | (d->steps & bit);
    ratchet_set(&home.slot[slot], s, g_step_ratchet(d, s));
    return state;
}
uint32_t glitch_mutes(void) { return g_mutes; }
void glitch_toggle_mute(unsigned slot)
{
    if (slot >= G_SLOTS) return;
    g_mutes ^= 1U << slot;
    home.mutes = g_mutes;
}
uint32_t glitch_activity(void)
{
    uint32_t bits = g_activity;
    g_activity = 0;
    uint32_t shown = bits | activity_seen;
    activity_seen = bits;
    return shown;
}

/* ------------------------------------------------------------ globals */
unsigned glitch_global_get(unsigned p) { return p < GG_COUNT ? home.global[p] : 0; }
const char *glitch_global_name(unsigned p) { return p < GG_COUNT ? global_params[p].name : ""; }
void glitch_global_text(unsigned p, char out[12])
{
    unsigned v = glitch_global_get(p);
    const uint8_t *g = home.global;
    switch (p) {
    case GG_BPM: put_number(out, (int)(40U + v)); return;
    case GG_SWING: put_number(out, (int)(50U + v * 25U / 99U)); strcat(out, "%"); return;
    case GG_MUTATE: if (!v) { strcpy(out, "OFF"); return; } put_number(out, (int)v); return;
    case GG_TAPE_FEEDBACK: put_number(out, (int)v); strcat(out, "%"); return;
    case GG_TAPE_SYNC: case GG_DELAY_SYNC: case GG_LFO_SYNC: strcpy(out, v ? "SYNC" : "FREE"); return;
    case GG_TAPE_TIME:
        if (g[GG_TAPE_SYNC]) { strcpy(out, echo_notes[note_index(v, ECHO_NOTES)].name); return; }
        seconds(out, tape_time_s()); return;
    case GG_DELAY_TIME:
        if (g[GG_DELAY_SYNC]) { strcpy(out, echo_notes[note_index(v, ECHO_NOTES)].name); return; }
        seconds(out, delay_time_s()); return;
    case GG_LFO_RATE: {
        if (g[GG_LFO_SYNC]) { strcpy(out, lfo_notes[note_index(v, LFO_NOTES)].name); return; }
        unsigned t = (unsigned)(lfo_rate_hz() * 100.0f + 0.5f);
        put_number(out, (int)(t / 100U)); strcat(out, ".");
        if (t % 100U < 10U) strcat(out, "0");
        put_number(out + strlen(out), (int)(t % 100U)); strcat(out, "HZ"); return;
    }
    default: put_number(out, (int)v); return;
    }
}
void glitch_global_edit(unsigned p, int delta)
{
    if (p >= GG_COUNT || !delta) return;
    int v = (int)home.global[p] + delta, max = global_params[p].max;
    v = v < 0 ? 0 : v > max ? max : v;
    if ((uint8_t)v == home.global[p]) return;
    home.global[p] = (uint8_t)v;
    derive();
}

/* ------------------------------------------------------------ building patterns */
static uint32_t dice;
static unsigned roll(unsigned n) { dice = dice * 1664525U + 1013904223U; return (dice >> 8) % n; }
static unsigned between(unsigned lo, unsigned hi) { return lo + roll(hi - lo + 1U); }
/* Exactly `hits` steps spread as evenly as possible over `length` (Euclid), rotated. */
static uint64_t euclid(unsigned hits, unsigned length, unsigned rotate)
{
    uint64_t mask = 0;
    if (!length) return 0;
    for (unsigned i = 0; i < length; ++i)
        if ((i * hits) % length < hits) mask |= (uint64_t)1 << ((i + rotate) % length);
    return mask;
}
static uint64_t every(unsigned spacing, unsigned length, unsigned offset)
{
    uint64_t mask = 0;
    for (unsigned i = offset; i < length; i += spacing) mask |= (uint64_t)1 << i;
    return mask;
}
static void slot_set(struct g_slot *d, unsigned source, uint64_t steps, unsigned length)
{
    uint8_t *v = d->value;
    memset(d, 0, sizeof *d);
    d->steps = steps;
    v[GP_SOURCE] = (uint8_t)source; v[GP_PITCH] = 24; v[GP_DECAY] = 35; v[GP_COLOR] = 50; v[GP_LEVEL] = 50;
    v[GP_PROB] = 100; v[GP_LENGTH] = (uint8_t)length; v[GP_RATE] = 0; v[GP_RANDOM] = 25;
    v[GP_PAN_MODE] = PAN_RANDOM; v[GP_PAN] = 40; v[GP_TAPE] = 5; v[GP_SHIMMER] = 20;
}
static void default_globals(uint8_t *g)
{
    static const uint8_t base[GG_COUNT] = {
        50, 0, 0, 80,      60, 25, 0, 50,
        99, 0, 30, 0,      50, 35, 30, 8,
        55, 30, 8, 0,      60, 45, 20, 15,
        1, 1, 1, 35};
    memcpy(g, base, GG_COUNT);
}
/* Pattern 1: a calm microsound groove after the Keinseier references:
 * micro kick, snap and hat as the frame, clicks, pops and grains around it. */
static void factory_micro(struct g_record *r)
{
    struct g_slot *s = r->slot;
    for (unsigned i = 0; i < G_SLOTS; ++i) slot_set(&s[i], GS_IMPULSE + i % (GS_DROP - 1U), 0, 16);
    slot_set(&s[0], GS_THUMP, 0x0481, 16);  /* F: kick on 1, 8, 11 */
    s[0].value[GP_LEVEL] = 80; s[0].value[GP_DECAY] = 15; s[0].value[GP_COLOR] = 25; s[0].value[GP_PAN_MODE] = PAN_FIXED;
    s[0].value[GP_PAN] = 50; s[0].value[GP_SHIMMER] = 0; s[0].value[GP_TAPE] = 0; s[0].value[GP_RANDOM] = 8;
    slot_set(&s[2], GS_SNAP, 0x1010, 16);   /* G: snap on 2 and 4, a roll into the second */
    s[2].value[GP_LEVEL] = 62; s[2].value[GP_DECAY] = 40; s[2].value[GP_COLOR] = 60; s[2].value[GP_PAN_MODE] = PAN_FIXED;
    s[2].value[GP_PAN] = 46; s[2].value[GP_SHIMMER] = 25;
    slot_set(&s[G_KEY_HAT], GS_HAT, 0x5555, 16); s[G_KEY_HAT].value[GP_LEVEL] = 36; s[G_KEY_HAT].value[GP_PROB] = 92;
    slot_set(&s[1], GS_IMPULSE, 0x2084, 16); s[1].value[GP_DECAY] = 30; s[1].value[GP_LEVEL] = 45;
    slot_set(&s[3], GS_POP, 0x0200, 16); s[3].value[GP_LEVEL] = 50; s[3].value[GP_DECAY] = 30;
    s[3].value[GP_PAN_MODE] = PAN_JUMP; s[3].value[GP_PAN] = 80;
    slot_set(&s[5], GS_TRAIN, 0x0808, 16); s[5].value[GP_DECAY] = 20; s[5].value[GP_LEVEL] = 44;
    s[5].value[GP_PAN_MODE] = PAN_JUMP; s[5].value[GP_PAN] = 70;
    slot_set(&s[6], GS_CRUNCH, euclid(3, 12, 2), 12); s[6].value[GP_LEVEL] = 36; s[6].value[GP_SHIMMER] = 45; s[6].value[GP_TAPE] = 20;
    slot_set(&s[7], GS_TICK, 0, 16); s[7].value[GP_SCATTER] = 3; s[7].value[GP_PROB] = 80; s[7].value[GP_LEVEL] = 35;
    slot_set(&s[8], GS_DROP, (uint64_t)1 << 14, 16); s[8].value[GP_PROB] = 35; s[8].value[GP_LEVEL] = 70; s[8].value[GP_DECAY] = 25;
    slot_set(&s[9], GS_GRAIN, euclid(2, 10, 3), 10); s[9].value[GP_LEVEL] = 38;
    slot_set(&s[10], GS_SNAP, euclid(1, 24, 6), 24); s[10].value[GP_LEVEL] = 32; s[10].value[GP_SHIMMER] = 50; s[10].value[GP_REVERSE] = 100;
    slot_set(&s[11], GS_HIGH, euclid(2, 24, 5), 24); s[11].value[GP_LEVEL] = 26; s[11].value[GP_DECAY] = 40;
    s[11].value[GP_PAN_MODE] = PAN_FIXED; s[11].value[GP_PAN] = 97;
    slot_set(&s[12], GS_LOOP, (uint64_t)1 << 13, 32); s[12].value[GP_LEVEL] = 34; s[12].value[GP_DECAY] = 50;
    s[12].value[GP_PAN_MODE] = PAN_FIXED; s[12].value[GP_PAN] = 3;
    s[1].value[GP_PAN_MODE] = PAN_PINGPONG; s[1].value[GP_PAN] = 95;
    ratchet_set(&s[2], 11, 2);               /* X3 on step 12: a roll into the snap */
    s[2].steps |= (uint64_t)1 << 11;
    for (unsigned i = 13; i < G_SLOTS; ++i) {
        s[i].value[GP_PITCH] = (uint8_t)(18U + (i * 7U) % 13U);
        s[i].value[GP_DECAY] = (uint8_t)(20U + (i * 17U) % 60U);
    }
    r->seed = 1U;
}
/* Roles in a random beat. KICK, SNARE and the hat are the frame; the others
 * are quieter glitch layers around it. */
enum { ROLE_KICK, ROLE_SNARE, ROLE_CLICKS, ROLE_COUNTER, ROLE_SPARSE, ROLE_TEXTURE, ROLE_ACCENT, ROLE_FAST, ROLE_HIGH, ROLE_DROP, ROLE_PLAY };
static void random_voice(struct g_slot *d, unsigned role)
{
    static const uint8_t pans[10] = {PAN_RANDOM, PAN_RANDOM, PAN_RANDOM, PAN_RANDOM, PAN_RANDOM, PAN_RANDOM,
                                     PAN_PINGPONG, PAN_PINGPONG, PAN_LFO, PAN_FIXED};
    static const uint8_t odd[6] = {16, 12, 14, 10, 20, 24};
    /* Kick lines: 1 and somewhere off the snare (bit n = step n + 1). */
    static const uint16_t kicks[8] = {0x0401, 0x0481, 0x0841, 0x0201, 0x0405, 0x4801, 0x0109, 0x2081};
    /* Snare lines: 2 and 4, half time, with a late ghost, displaced. */
    static const uint16_t snares[6] = {0x1010, 0x1010, 0x0100, 0x9010, 0x2010, 0x1210};
    unsigned length = 16, source = GS_IMPULSE;
    uint64_t steps = 0;
    unsigned prob = 100, ratchet = 0, scatter = 0, rate = 0, pan_mode = pans[roll(10)], pan = between(35, 75);
    unsigned level = between(36, 52), decay = between(10, 50), shimmer = between(0, 15), tape = between(0, 10);
    unsigned color = between(15, 85), pitch = between(18, 30), micro = between(0, 20);
    switch (role) {
    case ROLE_KICK:    /* a micro kick, centred, dry */
        source = GS_THUMP; steps = kicks[roll(8)];
        level = between(72, 86); decay = between(5, 30); color = between(0, 60); pitch = between(18, 30);
        pan_mode = PAN_FIXED; pan = 50; shimmer = between(0, 5); tape = 0; micro = 0; break;
    case ROLE_SNARE:   /* snap, tick or crunch on the backbeat */
        source = (uint8_t[]){GS_SNAP, GS_SNAP, GS_SNAP, GS_TICK, GS_CRUNCH}[roll(5)];
        steps = snares[roll(6)];
        level = between(58, 72); decay = between(30, 60); color = between(40, 80);
        pan_mode = PAN_FIXED; pan = between(42, 58); shimmer = between(5, 25); micro = between(0, 8); break;
    case ROLE_CLICKS:  /* syncopated clicks and pops on the sixteenths */
        source = (uint8_t[]){GS_IMPULSE, GS_IMPULSE, GS_POP, GS_TICK}[roll(4)];
        steps = euclid(between(3, 5), 16, roll(16)) & ~(uint64_t)1; prob = between(80, 100); break;
    case ROLE_COUNTER: /* a line in another length: the loops drift */
        source = (uint8_t[]){GS_TRAIN, GS_BITS, GS_GRAIN, GS_CRUNCH}[roll(4)];
        length = odd[1 + roll(5)]; steps = euclid(between(2, 5), length, roll(length));
        prob = between(75, 95); ratchet = roll(8) ? 0 : 1U; level = between(34, 48); break;
    case ROLE_SPARSE:  /* crunches, snaps, pops and short loops in long, sparse lines */
        source = (uint8_t[]){GS_CRUNCH, GS_SNAP, GS_POP, GS_LOOP, GS_LOOP}[roll(5)]; length = odd[roll(6)];
        steps = euclid(between(1, 2), length, roll(length));
        prob = between(70, 95); level = between(34, 48); shimmer = between(15, 45); tape = between(5, 25); break;
    case ROLE_TEXTURE: /* dust and grains, scattered */
        source = roll(2) ? GS_DUST : GS_GRAIN; scatter = between(1, 4); prob = between(60, 85);
        level = between(28, 40); break;
    case ROLE_ACCENT:  /* rare chirps and zaps */
        source = roll(2) ? GS_CHIRP : GS_ZAP; length = roll(2) ? 32 : 24; steps = euclid(1, length, roll(length));
        prob = between(60, 90); level = between(30, 44); tape = between(10, 35); break;
    case ROLE_FAST:    /* short 1/32 bursts */
        source = roll(2) ? GS_BITS : GS_TICK; rate = 1; length = 32; steps = euclid(between(2, 5), 32, roll(32));
        prob = between(55, 80); level = between(30, 40); decay = between(5, 25); break;
    case ROLE_HIGH:    /* very high beeps, short and quiet */
        source = GS_HIGH; length = (uint8_t[]){16, 24, 32, 12}[roll(4)]; steps = euclid(between(1, 3), length, roll(length));
        prob = between(60, 90); level = between(26, 36); decay = between(5, 45); color = roll(3) ? 0 : between(10, 60);
        pitch = between(14, 34); break;
    case ROLE_DROP:
        source = GS_DROP; steps = euclid(1, 16, between(5, 15)); prob = between(30, 60);
        level = between(50, 80); decay = between(10, 40); break;
    default:           /* a sound to play, without a pattern */
        source = between(GS_IMPULSE, GS_HIGH); break;
    }
    /* Many glitch layers sit hard left or right (the frame stays centred). */
    if (role != ROLE_KICK && role != ROLE_SNARE && roll(10) < 6) {
        pan_mode = (uint8_t[]){PAN_FIXED, PAN_FIXED, PAN_RANDOM, PAN_PINGPONG, PAN_JUMP, PAN_JUMP}[roll(6)];
        pan = pan_mode == PAN_FIXED ? (roll(2) ? between(0, 6) : between(93, 99))
            : pan_mode == PAN_JUMP ? between(25, 70) : between(88, 99);
    }
    slot_set(d, source, steps, length);
    uint8_t *v = d->value;
    v[GP_PROB] = (uint8_t)prob; v[GP_RATCHET] = (uint8_t)ratchet; v[GP_SCATTER] = (uint8_t)scatter;
    v[GP_RATE] = (uint8_t)rate; v[GP_MICRO] = (uint8_t)micro;
    v[GP_LEVEL] = (uint8_t)level; v[GP_DECAY] = (uint8_t)decay; v[GP_COLOR] = (uint8_t)color;
    v[GP_PITCH] = (uint8_t)pitch; v[GP_RANDOM] = (uint8_t)(role == ROLE_KICK ? 8U : between(10, 40));
    v[GP_PAN_MODE] = (uint8_t)pan_mode; v[GP_PAN] = (uint8_t)pan;
    v[GP_TAPE] = (uint8_t)tape; v[GP_SHIMMER] = (uint8_t)shimmer;
    /* Some sparse sounds play backwards; some lines get a ratchet on a step;
     * the snare sometimes rolls into its last hit. */
    if ((role == ROLE_SPARSE || role == ROLE_ACCENT || role == ROLE_TEXTURE || role == ROLE_PLAY) && roll(4) == 0)
        v[GP_REVERSE] = (uint8_t)between(30, 100);
    if ((role == ROLE_CLICKS || role == ROLE_COUNTER || role == ROLE_FAST) && roll(3) == 0) {
        unsigned on = 0;
        for (unsigned i = 0; i < length; ++i) on += (unsigned)((steps >> i) & 1U);
        unsigned pick = on ? roll(on) : 0;
        for (unsigned i = 0; on && i < length; ++i)
            if (((steps >> i) & 1U) && pick-- == 0) { ratchet_set(d, i, role == ROLE_FAST ? 1U : between(1, 2)); break; }
    }
    if (role == ROLE_SNARE && roll(3) == 0) {
        unsigned last = 15;
        while (last && !((steps >> last) & 1U)) --last;
        if (last && !((steps >> (last - 1U)) & 1U)) { d->steps |= (uint64_t)1 << (last - 1U); ratchet_set(d, last - 1U, between(2, 3)); }
    }
}
/* Expected hits per second of one slot (trains count once). */
static float slot_rate(const struct g_slot *d, float bar_s)
{
    static const uint8_t ratchets[5] = {1, 2, 3, 4, 6};
    const uint8_t *v = d->value;
    unsigned length = v[GP_LENGTH] ? v[GP_LENGTH] : 1U, hits = 0, each = ratchets[v[GP_RATCHET] < 5 ? v[GP_RATCHET] : 0];
    for (unsigned i = 0; i < length; ++i)
        if ((d->steps >> i) & 1U) { unsigned r = g_step_ratchet(d, i); hits += r ? r + 1U : each; }
    float steps_per_bar = (float)(16U << v[GP_RATE]);
    float per_bar = (float)hits * steps_per_bar / (float)length + (float)((unsigned)v[GP_SCATTER] << v[GP_RATE]);
    return v[GP_SOURCE] == GS_DROP ? 0.0f : per_bar * (float)v[GP_PROB] / 100.0f / bar_s;
}
static unsigned free_slot(uint32_t used)
{
    for (unsigned tries = 0; tries < 64; ++tries) {
        unsigned slot = roll(G_SLOTS);
        if (!fixed_slot(slot) && !((used >> slot) & 1U)) return slot;
    }
    for (unsigned slot = 0; slot < G_SLOTS; ++slot)
        if (!fixed_slot(slot) && !((used >> slot) & 1U)) return slot;
    return G_SLOTS;
}
/* A whole new beat into r (its globals stay): a frame of kick, snare and
 * hat, then a few quieter glitch layers; busy layers are thinned out. */
static void build_random(struct g_record *r, uint32_t seed)
{
    dice = seed | 1U;
    struct g_slot *s = r->slot;
    r->seed = seed * 2654435761U + 1U;
    r->mutes = 0;
    float density = unit(r->global[GG_DENSITY]);
    static const uint16_t hats[6] = {0x5555, 0x5555, 0x4444, 0xffff, 0x5554, 0x6db6};
    slot_set(&s[G_KEY_HAT], GS_HAT, hats[roll(density > 0.5f ? 6 : 3)], 16);
    s[G_KEY_HAT].value[GP_LEVEL] = (uint8_t)between(26, 38);
    s[G_KEY_HAT].value[GP_PROB] = (uint8_t)between(80, 100);
    s[G_KEY_HAT].value[GP_COLOR] = (uint8_t)between(30, 80);
    s[G_KEY_HAT].value[GP_DECAY] = (uint8_t)between(5, 30);
    s[G_KEY_HAT].value[GP_MICRO] = (uint8_t)between(0, 12);
    if (density > 0.6f && roll(3) == 0) { s[G_KEY_HAT].value[GP_RATE] = 1; s[G_KEY_HAT].value[GP_LENGTH] = 32;
        s[G_KEY_HAT].steps = euclid(between(9, 15), 32, 0); }
    uint32_t used = 1U << G_KEY_HAT, frame = used;
    uint8_t roles[12];
    unsigned count = 0;
    roles[count++] = ROLE_KICK;
    roles[count++] = ROLE_SNARE;
    roles[count++] = ROLE_CLICKS;
    if (roll(10) < 7) roles[count++] = ROLE_COUNTER;
    if (roll(10) < 6) roles[count++] = ROLE_SPARSE;
    if (roll(10) < 5) roles[count++] = ROLE_TEXTURE;
    if (roll(10) < 3 + (unsigned)(density * 4.0f)) roles[count++] = ROLE_CLICKS;
    if (roll(10) < 3) roles[count++] = ROLE_ACCENT;
    if (roll(10) < (unsigned)(density * 6.0f)) roles[count++] = ROLE_FAST;
    if (roll(10) < 5) roles[count++] = ROLE_HIGH;
    if (roll(10) < 2) roles[count++] = ROLE_DROP;
    for (unsigned i = 0; i < count; ++i) {
        unsigned slot = free_slot(used);
        if (slot >= G_SLOTS) break;
        used |= 1U << slot;
        if (roles[i] == ROLE_KICK || roles[i] == ROLE_SNARE) frame |= 1U << slot;
        random_voice(&s[slot], roles[i]);
    }
    /* Leave room between the hits: thin out the busiest glitch layer (never
     * the frame) until the beat fits the density. */
    float bar = 240.0f / (float)(40U + r->global[GG_BPM]), target = 4.0f + 12.0f * density;
    for (unsigned guard = 0; guard < 96; ++guard) {
        float total = 0.0f, worst = 0.0f;
        unsigned busiest = G_SLOTS;
        for (unsigned slot = 0; slot < G_SLOTS; ++slot) {
            if (!((used >> slot) & 1U)) continue;
            float hits = slot_rate(&s[slot], bar);
            total += hits;
            if (!((frame >> slot) & 1U) && hits > worst) { worst = hits; busiest = slot; }
        }
        if (total <= target || busiest >= G_SLOTS) break;
        uint8_t *v = s[busiest].value;
        uint8_t *rat = s[busiest].ratchet;
        unsigned step_ratchets = 0;
        for (unsigned b = 0; b < G_STEPS / 4; ++b) step_ratchets |= rat[b];
        if (v[GP_SCATTER] > 1) v[GP_SCATTER] = (uint8_t)(v[GP_SCATTER] * 2U / 3U);
        else if (v[GP_RATCHET]) v[GP_RATCHET] = 0;
        else if (step_ratchets) memset(rat, 0, G_STEPS / 4);
        else s[busiest].steps &= s[busiest].steps - 1U; /* drop a step */
    }
    for (unsigned slot = 0; slot < G_SLOTS; ++slot)
        if (!((used >> slot) & 1U)) random_voice(&s[slot], ROLE_PLAY);
}
/* Factory pattern n: 1 is curated, the others are seeded random beats with
 * their own swing, accents, flams, density and effect colour. */
static void factory(unsigned n, struct g_record *r)
{
    memset(r, 0, sizeof *r);
    default_globals(r->global);
    if (n == 0) { factory_micro(r); return; }
    dice = n * 7919U + 17U;
    uint8_t *g = r->global;
    g[GG_DENSITY] = (uint8_t)between(20, 85);
    g[GG_SWING] = (uint8_t)(roll(3) ? 0 : between(20, 60));
    g[GG_MUTATE] = (uint8_t)(roll(2) ? 0 : between(15, 40));
    g[GG_ACCENT] = (uint8_t)between(40, 85);
    g[GG_FLAM] = (uint8_t)between(10, 45);
    /* Dry like the references: silence between the hits, a little space. */
    g[GG_REV_MIX] = (uint8_t)between(8, 25); g[GG_SHIMMER] = (uint8_t)between(5, 40);
    g[GG_TAPE_MIX] = (uint8_t)between(0, 15); g[GG_TAPE_FEEDBACK] = (uint8_t)between(20, 50);
    g[GG_DELAY_MIX] = (uint8_t)between(0, 15); g[GG_DELAY_TIME] = (uint8_t)between(25, 70);
    g[GG_FILTER] = (uint8_t)(roll(3) ? 99 : between(55, 85)); g[GG_RESO] = (uint8_t)(g[GG_FILTER] < 99 ? between(10, 50) : 0);
    g[GG_LFO_DEPTH] = (uint8_t)(g[GG_FILTER] < 99 ? between(5, 30) : 0);
    build_random(r, n * 2654435761U + 99U);
}

/* ------------------------------------------------------------ Flash */
static uint32_t record_crc(const struct g_record *r)
{
    return groove_crc32(~0U, (const uint8_t *)r + 12, (unsigned)(sizeof *r - 12U));
}
static int read_record(unsigned n, struct g_record *r)
{
    if (n >= G_PATTERNS || fm1_flash_size() < PATTERN_BASE + G_PATTERNS * FM1_FLASH_SECTOR_BYTES) return -1;
    if (fm1_flash_read(PATTERN_BASE + n * FM1_FLASH_SECTOR_BYTES, r, sizeof *r)) return -1;
    if (r->magic != RECORD_MAGIC || r->version != RECORD_VERSION || r->crc != record_crc(r)) return -1;
    return 0;
}
unsigned glitch_pattern_stored(unsigned n)
{
    uint32_t head[2];
    if (n >= G_PATTERNS || fm1_flash_size() < PATTERN_BASE + G_PATTERNS * FM1_FLASH_SECTOR_BYTES) return 0;
    if (fm1_flash_read(PATTERN_BASE + n * FM1_FLASH_SECTOR_BYTES, head, sizeof head)) return 0;
    return head[0] == RECORD_MAGIC && head[1] == RECORD_VERSION;
}
static struct g_record scratch;
int glitch_save(unsigned n)
{
    if (n >= G_PATTERNS) return -1;
    if (fm1_flash_size() < PATTERN_BASE + G_PATTERNS * FM1_FLASH_SECTOR_BYTES) return -1;
    /* What you hear becomes the pattern: the live loop and its mutes. */
    for (unsigned s = 0; s < G_SLOTS; ++s) home.slot[s] = g_live[s];
    home.mutes = g_mutes;
    home.seed = g_seed;
    home.magic = RECORD_MAGIC; home.version = RECORD_VERSION;
    home.crc = record_crc(&home);
    scratch = home;
    uint32_t at = PATTERN_BASE + n * FM1_FLASH_SECTOR_BYTES;
    int r = fm1_flash_erase(at);
    if (!r) r = fm1_flash_write(at, &scratch, sizeof scratch);
    if (!r) pattern_now = n;
    return r;
}

/* ------------------------------------------------------------ patterns */
static void apply_live_now(void)
{
    for (unsigned s = 0; s < G_SLOTS; ++s) g_live[s] = home.slot[s];
    g_seed = home.seed;
    g_mutes = home.mutes;
}
/* Load pattern n into `home`; while running it starts at the next bar. */
static void load(unsigned n)
{
    uint8_t bpm = home.global[GG_BPM];
    unsigned keep_tempo = g_run || glitch_bpm_external();
    if (read_record(n, &home)) factory(n, &home);
    if (keep_tempo) home.global[GG_BPM] = bpm;
    pattern_now = n;
    if (g_run) {
        for (unsigned s = 0; s < G_SLOTS; ++s) g_staged[s] = home.slot[s];
        g_staged_seed = home.seed;
        FX_BARRIER();
        g_staged_ready = 1;
        pending_globals = 1;
        pattern_next = (int)n;
    } else {
        apply_live_now();
        derive();
        pattern_next = -1;
    }
}
unsigned glitch_pattern(void) { return pattern_now; }
int glitch_pattern_next(void) { return pattern_next; }
void glitch_select_pattern(unsigned n) { if (n < G_PATTERNS) load(n); }
void glitch_randomize(uint32_t seed)
{
    build_random(&home, seed);
    if (g_run) {
        for (unsigned s = 0; s < G_SLOTS; ++s) g_staged[s] = home.slot[s];
        g_staged_seed = home.seed;
        FX_BARRIER();
        g_staged_ready = 1;
        pending_globals = 1;
    } else apply_live_now();
}
void glitch_randomize_slot(unsigned slot, uint32_t seed)
{
    if (slot >= G_SLOTS) return;
    dice = seed | 1U;
    static const uint8_t roles[7] = {ROLE_CLICKS, ROLE_COUNTER, ROLE_SPARSE, ROLE_TEXTURE, ROLE_HIGH, ROLE_FAST, ROLE_CLICKS};
    struct g_slot d;
    if (fixed_slot(slot)) {
        d = g_live[slot];
        d.steps = roll(2) ? every(2, 16, roll(2)) : euclid(between(5, 9), 16, roll(16));
        d.value[GP_LENGTH] = 16; d.value[GP_RATE] = 0;
        d.value[GP_COLOR] = (uint8_t)between(30, 80);
    } else random_voice(&d, g_live[slot].value[GP_SOURCE] == GS_THUMP ? ROLE_KICK : roles[roll(7)]);
    g_live[slot] = d;
    home.slot[slot] = d;
}
void glitch_reset_loop(void)
{
    for (unsigned s = 0; s < G_SLOTS; ++s) g_live[s] = home.slot[s];
}
unsigned glitch_mutated(void)
{
    return memcmp(g_live, home.slot, sizeof g_live) != 0;
}

/* ------------------------------------------------------------ living loops */
/* Turn the step ratchets with the steps (left: step i moves to i + 1). */
static void turn_ratchets(struct g_slot *d, unsigned length, unsigned left)
{
    uint8_t r[G_STEPS];
    for (unsigned i = 0; i < length; ++i) r[i] = (uint8_t)g_step_ratchet(d, i);
    for (unsigned i = 0; i < length; ++i)
        ratchet_set(d, left ? (i + 1U) % length : (i + length - 1U) % length, r[i]);
}
static unsigned count_steps(uint64_t steps, unsigned length)
{
    unsigned n = 0;
    for (unsigned i = 0; i < length; ++i) n += (unsigned)((steps >> i) & 1U);
    return n;
}
/* Once per bar: each playing slot may change a little. Steps flip, the
 * pattern turns by one step, ratchets and pitch move; the density stays
 * near the stored pattern, and with little MUTATE the loop drifts back. */
static void mutate(void)
{
    float m = unit(home.global[GG_MUTATE]);
    if (m <= 0.0f) return;
    for (unsigned slot = 0; slot < G_SLOTS; ++slot) {
        struct g_slot *live = &g_live[slot];
        const struct g_slot *base = &home.slot[slot];
        unsigned length = live->value[GP_LENGTH] ? live->value[GP_LENGTH] : 1U;
        uint64_t window = length >= 64 ? ~(uint64_t)0 : (((uint64_t)1 << length) - 1U);
        if (!base->steps && !base->value[GP_SCATTER]) continue; /* only slots that play */
        if ((float)roll(1000) < m * 350.0f) {
            unsigned op = roll(100);
            if (live->value[GP_SOURCE] == GS_THUMP && op < 70) op = 82; /* the kick line stays; its pitch may walk */
            uint64_t steps = live->steps;
            unsigned have = count_steps(steps, length), want = count_steps(base->steps, length);
            if (op < 55 && length > 1) {        /* flip a step, keeping the density */
                unsigned i = roll(length);
                uint64_t bit = (uint64_t)1 << i;
                if (have > want + 1U) { for (unsigned t = 0; t < 16 && !(steps & bit); ++t) bit = (uint64_t)1 << roll(length); steps &= ~bit; }
                else if (have + 1U < want || !have) { for (unsigned t = 0; t < 16 && (steps & bit); ++t) bit = (uint64_t)1 << roll(length); steps |= bit; }
                else steps ^= bit;
                if (!(steps & window) && base->steps) steps = base->steps;
            } else if (op < 70 && length > 1) { /* turn by one step */
                unsigned left = roll(2);
                steps = left ? ((steps << 1) | (steps >> (length - 1U))) & window
                             : ((steps >> 1) | ((steps & 1U) << (length - 1U))) & window;
                turn_ratchets(live, length, left);
            } else if (op < 82) {               /* ratchet on / off: one step, or the whole slot */
                unsigned i = roll(length);
                for (unsigned t = 0; t < 16 && !((steps >> i) & 1U); ++t) i = roll(length);
                if (roll(2) && ((steps >> i) & 1U))
                    ratchet_set(live, i, g_step_ratchet(live, i) ? 0 : between(1, 2));
                else live->value[GP_RATCHET] = live->value[GP_RATCHET] ? 0 : (uint8_t)between(1, 2);
            } else {                            /* pitch walks around the stored one */
                int p = (int)live->value[GP_PITCH] + (roll(2) ? 2 : -2), b = base->value[GP_PITCH];
                live->value[GP_PITCH] = (uint8_t)(p < b - 7 ? b - 7 : p > b + 7 ? b + 7 : p < 0 ? 0 : p > 48 ? 48 : p);
            }
            live->steps = steps;
        } else if ((float)roll(1000) < (1.0f - m) * 250.0f) {
            /* Drift back: one difference to the stored pattern is undone. */
            uint64_t diff = (live->steps ^ base->steps) & window;
            unsigned b = 0;
            while (b < G_STEPS / 4 && live->ratchet[b] == base->ratchet[b]) ++b;
            if (diff) {
                uint64_t bit = diff & (~diff + 1U);
                live->steps = (live->steps & ~bit) | (base->steps & bit);
            } else if (b < G_STEPS / 4) {
                live->ratchet[b] = base->ratchet[b];
            } else {
                live->value[GP_RATCHET] = base->value[GP_RATCHET];
                live->value[GP_PITCH] = base->value[GP_PITCH];
            }
        }
    }
    if ((float)roll(1000) < m * 120.0f) g_seed = g_seed * 1664525U + 1013904223U; /* new scatter places */
}

/* ------------------------------------------------------------ transport, perform */
void glitch_transport(int run) { g_run = (uint8_t)(run < 0 ? !g_run : run != 0); }
unsigned glitch_running(void) { return g_run; }
void glitch_perform(unsigned fx) { g_perform_now = (uint8_t)(fx < GF_COUNT ? fx : GF_OFF); }
unsigned glitch_performing(void) { return g_perform_now; }
void glitch_punch(unsigned fx) { g_punch_now = (uint8_t)(fx < GX_COUNT ? fx : GX_OFF); }
unsigned glitch_punching(void) { return g_punch_now; }

/* ------------------------------------------------------------ tempo */
static uint32_t tap_last, tap_intervals[3];
static unsigned tap_count;
static volatile uint32_t clock_last, clock_beat_start, clock_ticks;
static volatile float clock_bpm;
static volatile uint8_t clock_running, transport_request; /* 1 start, 2 stop, 3 continue */
static void set_bpm(float bpm)
{
    int v = (int)(bpm + 0.5f) - 40;
    v = v < 0 ? 0 : v > global_params[GG_BPM].max ? global_params[GG_BPM].max : v;
    if ((uint8_t)v == home.global[GG_BPM]) return;
    home.global[GG_BPM] = (uint8_t)v;
    derive();
}
unsigned glitch_bpm(void) { return 40U + home.global[GG_BPM]; }
unsigned glitch_bpm_external(void) { return clock_running; }
void glitch_tap(uint32_t now)
{
    uint32_t gap = now - tap_last;
    tap_last = now;
    if (gap < 200U || gap > 2000U) { tap_count = 0; return; } /* 30..300 BPM */
    tap_intervals[tap_count % 3U] = gap;
    if (++tap_count < 2) return;
    unsigned n = tap_count < 3 ? tap_count : 3U;
    uint32_t sum = 0;
    for (unsigned i = 0; i < n; ++i) sum += tap_intervals[i];
    set_bpm(60000.0f * (float)n / (float)sum);
}
/* MIDI input context: time 24 clock ticks per beat; Start/Continue/Stop
 * drive the transport. The control task applies both. */
void glitch_midi_realtime(uint8_t status, uint32_t now)
{
    if (status == 0xfa) { transport_request = 1; clock_ticks = 0; return; }
    if (status == 0xfb) { transport_request = 3; return; }
    if (status == 0xfc) { clock_running = 0; clock_ticks = 0; transport_request = 2; return; }
    if (status != 0xf8) return;
    if (now - clock_last > 2000U) clock_ticks = 0;
    clock_last = now;
    if (clock_ticks++ == 0) { clock_beat_start = now; return; }
    if (clock_ticks == 25U) {
        uint32_t beat = now - clock_beat_start;
        if (beat >= 150U && beat <= 2000U) { clock_bpm = 60000.0f / (float)beat; clock_running = 1; }
        clock_beat_start = now;
        clock_ticks = 1;
    }
}
/* ------------------------------------------------------------ MIDI notes */
static struct { volatile unsigned read, write; uint8_t data[64][3]; } midi_in;
void glitch_midi_message(const uint8_t *m, size_t n)
{
    if (!m || n < 2 || m[0] < 0x80 || m[0] >= 0xf0) return;
    unsigned w = midi_in.write;
    if (w - midi_in.read >= 64U) return;
    midi_in.data[w & 63U][0] = m[0];
    midi_in.data[w & 63U][1] = m[1];
    midi_in.data[w & 63U][2] = n > 2 ? m[2] : 0;
    FX_BARRIER();
    midi_in.write = w + 1U;
}
static void midi_play(const uint8_t m[3])
{
    unsigned type = m[0] & 0xf0U;
    if (type == 0x90 && m[2] && m[1] >= 36) glitch_hit((m[1] - 36U) % G_SLOTS); /* C1 = the F key */
}

/* ------------------------------------------------------------ poll, init */
void glitch_poll(uint32_t now)
{
    if (clock_running && now - clock_last > 2000U) clock_running = 0;
    if (clock_running) set_bpm(clock_bpm);
    unsigned request = transport_request;
    if (request) {
        transport_request = 0;
        glitch_transport(request == 2 ? 0 : 1);
    }
    for (unsigned r = midi_in.read, w = midi_in.write; r != w; ++r) {
        FX_BARRIER();
        midi_play(midi_in.data[r & 63U]);
        midi_in.read = r + 1U;
    }
    /* A queued pattern took over at the bar start: its settings follow. */
    if (pending_globals && !g_staged_ready) {
        pending_globals = 0;
        g_mutes = home.mutes;
        pattern_next = -1;
        derive();
    }
    uint32_t bars = g_bars;
    if (bars != bars_seen) {
        bars_seen = bars;
        if (g_run && !g_staged_ready) mutate();
    }
}
void glitch_init(void)
{
    dice = 0x2545f491U;
    load(0);
    g_run = 0;
}
