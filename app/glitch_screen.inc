/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception */
/* GLITCH panel. Included by screen.c (via glitch_ui.inc) to reuse its LCD
 * painter.
 *
 * Keys:     play and select a slot (A = classic hi-hat, others microsounds)
 * SEQ held + white keys: steps of the selected slot (16 per page); pressing
 *           a step again gives it a ratchet: on -> X2 -> X3 -> X4 -> off;
 *           OCT- / OCT+ step page 1-4 (64 steps)
 * PLAY      start / stop            SEL + PLAY  tap tempo
 * ARP       a completely new beat   ARP held + key  a new sound and line for that slot
 * REC       undo the mutations (back to the stored pattern)
 * GLO held + key  mute / unmute     GLO alone   unmute all
 * FX        effect pages            FX held + white key  momentary stutter effect
 *           FX held + black key  punch-in: ratchet X2 / X3 / X4, roll, reverse all,
 *           reverse 1/16, scratch, shuffle, octave up, stutter, chaos
 * HOME      tempo, swing, mutate, level; HOME held + white key  load pattern 1-16
 * EDIT      slot pages (sound, rhythm, loop, space, reverse)   ENV slot sound   LFO filter
 * SEL       kit page (accent, flam, hiss, density)
 * SAVE      store the live loop in its pattern; SAVE held + white key  store in pattern 1-16
 * Knobs:    K1-K4 edit the page, SELECT slot, PRESETS pattern (next bar while running) */
#include "glitch.h"
#ifndef FM1_HOST_TEST
unsigned fm1_audio_load_percent(void); /* app/board/audio.c */
#endif
static unsigned g_cpu_load(void)
{
#ifdef FM1_HOST_TEST
    return 0; /* the browser has no interrupt budget to show */
#else
    return fm1_audio_load_percent();
#endif
}
enum { RED = 0xa8a3, DARK = 0x0861 };
enum { GLOBAL = 100, NONE = 255 }; /* page entries >= GLOBAL are global settings; NONE an empty knob */
enum { PAGE_HOME, PAGE_SOUND, PAGE_RHYTHM, PAGE_LOOP, PAGE_SPACE, PAGE_REVERSE, PAGE_KIT,
       PAGE_FILTER, PAGE_TAPE, PAGE_DELAY, PAGE_REVERB, PAGE_SYNC, PAGES };
static const struct { char title[20]; uint8_t p[4]; } g_pages[PAGES] = {
    {"HOME", {GLOBAL + GG_BPM, GLOBAL + GG_SWING, GLOBAL + GG_MUTATE, GLOBAL + GG_LEVEL}},
    {"SLOT 1/5 SOUND", {GP_SOURCE, GP_PITCH, GP_DECAY, GP_COLOR}},
    {"SLOT 2/5 RHYTHM", {GP_PROB, GP_RATCHET, GP_MICRO, GP_SCATTER}},
    {"SLOT 3/5 LOOP", {GP_LENGTH, GP_RATE, GP_RANDOM, GP_LEVEL}},
    {"SLOT 4/5 SPACE", {GP_PAN_MODE, GP_PAN, GP_TAPE, GP_SHIMMER}},
    {"SLOT 5/5 REVERSE", {GP_REVERSE, NONE, NONE, NONE}},
    {"KIT", {GLOBAL + GG_ACCENT, GLOBAL + GG_FLAM, GLOBAL + GG_HISS, GLOBAL + GG_DENSITY}},
    {"FX 1/5 FILTER", {GLOBAL + GG_FILTER, GLOBAL + GG_RESO, GLOBAL + GG_LFO_RATE, GLOBAL + GG_LFO_DEPTH}},
    {"FX 2/5 TAPE", {GLOBAL + GG_TAPE_TIME, GLOBAL + GG_TAPE_FEEDBACK, GLOBAL + GG_TAPE_WOW, GLOBAL + GG_TAPE_MIX}},
    {"FX 3/5 DELAY", {GLOBAL + GG_DELAY_TIME, GLOBAL + GG_DELAY_FEEDBACK, GLOBAL + GG_DELAY_MIX, GLOBAL + GG_DRIVE}},
    {"FX 4/5 SHIMMER", {GLOBAL + GG_REV_SIZE, GLOBAL + GG_REV_DAMP, GLOBAL + GG_SHIMMER, GLOBAL + GG_REV_MIX}},
    {"FX 5/5 SYNC", {GLOBAL + GG_TAPE_SYNC, GLOBAL + GG_DELAY_SYNC, GLOBAL + GG_LFO_SYNC, GLOBAL + GG_TAPE_TONE}},
};
/* Physical keys 0..26 are F3..G5. */
static const uint8_t g_white_key[16] = {0, 2, 4, 6, 7, 9, 11, 12, 14, 16, 18, 19, 21, 23, 24, 26};
static int g_white_of(unsigned key)
{
    for (unsigned i = 0; i < 16; ++i) if (g_white_key[i] == key) return (int)i;
    return -1;
}
/* FX held + key: white keys 1-16 are the stutter bank, the eleven black keys
 * the punch-in effects. Values >= PUNCH are sequencer effects (enum g_punch). */
enum { PUNCH = 100 };
static const uint8_t g_black_key[11] = {1, 3, 5, 8, 10, 13, 15, 17, 20, 22, 25};
static const uint8_t g_black_fx[11] = {
    PUNCH + GX_RATCHET_2, PUNCH + GX_RATCHET_3, PUNCH + GX_RATCHET_4, PUNCH + GX_ROLL, PUNCH + GX_REVERSE,
    GF_REVERSE_16, GF_SCRATCH, GF_SHUFFLE, GF_OCTAVE, GF_STUTTER, PUNCH + GX_CHAOS};
static unsigned g_key_effect(unsigned key)
{
    int white = g_white_of(key);
    if (white >= 0) return GF_REPEAT_4 + (unsigned)white;
    for (unsigned i = 0; i < 11; ++i) if (g_black_key[i] == key) return g_black_fx[i];
    return GF_OFF;
}
static unsigned g_page = PAGE_HOME, g_ready, g_full, g_slot = 0, g_step_page;
static uint32_t g_keys_before, g_buttons_before, g_frame_ms, g_notice_until, g_sig[5];
static const char *g_notice;
static char g_notice_text[24];
static unsigned seq_used, fx_used, arp_used, glo_used, save_used, home_used, fx_key = 255, punch_key = 255;
static uint32_t g_activity_shown;
/* Regions to repaint (bit 0 header, 1 steps, 2 keyboard, 3 title). The
 * band pass draws everything; the clip keeps it to the band being sent. */
static unsigned g_dirty;
#define G_REGION(n) (paint_rendering || ((g_dirty >> (n)) & 1U))
static void g_set_notice(const char *s, uint32_t now) { g_notice = s; g_notice_until = now + 1500U; }
static void g_pattern_notice(const char *what, unsigned pattern, uint32_t now)
{
    char n[4] = {(char)('0' + (pattern + 1U) / 10U), (char)('0' + (pattern + 1U) % 10U), 0};
    strcpy(g_notice_text, what); strcat(g_notice_text, " P"); strcat(g_notice_text, n);
    g_set_notice(g_notice_text, now);
}
static void g_param_text(unsigned entry, char label[12], char value[12])
{
    if (entry == NONE) { label[0] = value[0] = 0; return; }
    if (entry >= GLOBAL) {
        strcpy(label, glitch_global_name(entry - GLOBAL));
        glitch_global_text(entry - GLOBAL, value);
    } else {
        strcpy(label, glitch_slot_param_name(entry));
        glitch_slot_text(g_slot, entry, value);
    }
}
static void g_key_name(unsigned key, char out[4])
{
    static const char notes[12][3] = {"F", "F#", "G", "G#", "A", "A#", "B", "C", "C#", "D", "D#", "E"};
    strcpy(out, notes[key % 12U]);
}
static void g_draw(void)
{
    char line[32], value[12];
    if (paint_rendering) paint_fill(0, paint_top, FM1_SCREEN_WIDTH, PAINT_ROWS, BACK); /* compose each band from scratch */
    unsigned running = glitch_running(), pattern = glitch_pattern();
    int next = glitch_pattern_next();
    unsigned length = glitch_slot_get(g_slot, GP_LENGTH);
    /* Header: pattern, transport, tempo. */
    if (G_REGION(0)) {
    line[0] = 'P'; line[1] = (char)('0' + (pattern + 1U) / 10U); line[2] = (char)('0' + (pattern + 1U) % 10U); line[3] = 0;
    text(6, 6, line, 2, ACCENT, BACK);
    text(50, 6, running ? "RUN" : "STOP", 1, running ? GREEN : DIM, BACK);
    if (next >= 0) {
        strcpy(line, "NEXT P"); line[6] = (char)('0' + (next + 1) / 10); line[7] = (char)('0' + (next + 1) % 10); line[8] = 0;
        text(50, 16, line, 1, AMBER, BACK);
    } else if (glitch_mutated()) text(50, 16, "LIVE", 1, AMBER, BACK);
    if (glitch_global_get(GG_MUTATE)) text(104, 6, "MUTATE", 1, GREEN, BACK);
    if (glitch_punching()) text(104, 16, glitch_punch_name(glitch_punching()), 1, AMBER, BACK);
    else if (glitch_performing()) text(104, 16, glitch_perform_name(glitch_performing()), 1, AMBER, BACK);
    glitch_global_text(GG_BPM, value);
    text(186, 4, value, 2, glitch_bpm_external() ? GREEN : AMBER, BACK);
    text(186, 20, glitch_bpm_external() ? "EXT" : "BPM", 1, DIM, BACK);
    /* Selected slot: key, sound, loop length and rate. */
    g_key_name(g_slot, line);
    strcat(line, " ");
    glitch_slot_name(g_slot, value);
    strcat(line, value);
    text(6, 30, line, 2, TEXT, BACK);
    char rate[12];
    glitch_slot_text(g_slot, GP_RATE, rate);
    line[0] = 0;
    if (length >= 10) { line[0] = (char)('0' + length / 10U); line[1] = 0; }
    { char d[2] = {(char)('0' + length % 10U), 0}; strcat(line, d); }
    strcat(line, " "); strcat(line, rate);
    text(180, 34, line, 1, DIM, BACK);
    }
    /* 64 steps in four rows of 16; the edited page is marked on the right. */
    if (G_REGION(1)) {
    uint64_t steps = glitch_steps(g_slot);
    unsigned now_step = glitch_position(g_slot);
    for (unsigned s = 0; s < G_STEPS; ++s) {
        unsigned row = s / 16U, col = s % 16U, x = 2U + col * 14U, y = 50U + row * 15U, on = (steps >> s) & 1U;
        uint16_t c = s >= length ? DARK : on ? (col % 4U ? GREEN : ACCENT) : PANEL;
        paint_fill(x, y, 13, 13, running && s == now_step && s < length ? AMBER : BACK);
        paint_fill(x + 1U, y + 1U, 11, 11, c);
        /* A step ratchet splits the step into its 2, 3 or 4 hits. */
        static const uint8_t cuts[4][3] = {{0}, {6}, {4, 8}, {3, 6, 9}};
        unsigned r = on ? glitch_step_ratchet(g_slot, s) : 0;
        for (unsigned k = 0; k < r; ++k) paint_fill(x + cuts[r][k], y + 1U, 1, 11, BACK);
    }
    for (unsigned row = 0; row < 4; ++row)
        paint_fill(228, 52U + row * 15U, 6, 9, row == g_step_page ? AMBER : BACK);
    }
    /* Keyboard: slots that just played flash, muted ones are red, the
     * selected one is outlined. */
    if (G_REGION(2)) {
    uint32_t mutes = glitch_mutes();
    for (unsigned key = 0; key < G_SLOTS; ++key) {
        unsigned x = 3U + key * 8U, black = g_white_of(key) < 0;
        uint16_t c = (g_activity_shown >> key) & 1U ? AMBER : (mutes >> key) & 1U ? RED : black ? DIM : PANEL;
        paint_fill(x - 1U, black ? 112 : 122, 9, black ? 14 : 30, key == g_slot ? TEXT : BACK);
        paint_fill(x, black ? 113 : 123, 7, black ? 12 : 28, c);
    }
    }
    /* Page title or a notice, and a hint. */
    if (G_REGION(3)) {
    text(6, 160, g_notice ? g_notice : g_pages[g_page].title, g_notice ? 1 : 2, g_notice ? AMBER : TEXT, BACK);
    if (!g_notice) {
        if (g_page == PAGE_HOME) {
            char load[32] = "CPU ", pct[8];
            unsigned l = g_cpu_load(), n = 0;
            do { pct[n++] = (char)('0' + l % 10U); l /= 10U; } while (l && n < 6);
            while (n) { unsigned len = (unsigned)strlen(load); load[len] = pct[--n]; load[len + 1] = 0; }
            strcat(load, "%  ARP = NEW BEAT");
            text(6, 180, load, 1, g_cpu_load() > 85U ? AMBER : DIM, BACK);
        } else if (g_page == PAGE_REVERSE) text(6, 180, "DECAY SETS THE LENGTH OF THE SWELL", 1, DIM, BACK);
        else if (g_page >= PAGE_FILTER) text(6, 180, "HOLD FX + KEY = EFFECT", 1, DIM, BACK);
        else text(6, 180, "SEQ + WHITE = STEPS  OCT = PAGE", 1, DIM, BACK);
    }
    }
    for (unsigned col = 0; col < 4; ++col) {
        char label[12];
        g_param_text(g_pages[g_page].p[col], label, value);
        knob(col, label, value, colors[col * 2U]);
    }
}
/* LEDs: keys show the edited step page, or mutes / patterns / the stutter
 * key while GLO / HOME / FX is held; PLAY lights while running. */
static void g_leds(const struct fm1_controls *c)
{
    static uint32_t shown = ~0U;
    static unsigned buttons_shown = ~0U;
    uint32_t bits = 0;
    if (c->buttons & BUTTON(GLO)) bits = glitch_mutes();
    else if (c->buttons & BUTTON(HOME)) bits = 1U << g_white_key[glitch_pattern() & 15U];
    else if ((c->buttons & BUTTON(FX)) && (fx_key < 27U || punch_key < 27U)) bits = (fx_key < 27U ? 1U << fx_key : 0U) | (punch_key < 27U ? 1U << punch_key : 0U);
    else {
        uint64_t steps = glitch_steps(g_slot) >> (g_step_page * 16U);
        for (unsigned i = 0; i < 16; ++i) if ((steps >> i) & 1U) bits |= 1U << g_white_key[i];
        bits |= 1U << g_slot;
    }
    if (bits != shown) {
        for (unsigned key = 0; key < 27; ++key)
            if (((bits ^ shown) >> key) & 1U) fm1_led_set(key + 14U, (int)((bits >> key) & 1U));
        shown = bits;
    }
    unsigned b = (glitch_running() ? 1U : 0U) | (glitch_mutated() ? 2U : 0U);
    if (b != buttons_shown) {
        fm1_led_set(PLAY, (int)(b & 1U));
        fm1_led_set(REC, (int)((b >> 1) & 1U));
        buttons_shown = b;
    }
}
static void g_control(const struct fm1_controls *c)
{
    uint32_t now = fm1_millis();
    if (!g_ready) {
        g_ready = g_full = 1;
        /* The FM1 tracks are not used: switch them off so the SDK releases
         * their sound engines (heap and audio time). A device preference. */
        for (unsigned t = 0; t < GROOVE_TRACKS; ++t)
            if (groove_track_role(t) != GROOVE_TRACK_OFF) (void)groove_track_role_set(t, GROOVE_TRACK_OFF);
        for (unsigned key = 0; key < 41; ++key) fm1_led_set(key, 0);
        g_keys_before = c->keys; /* keys held at boot do nothing */
        g_set_notice("GLITCH", now);
    }
    unsigned changed = groove_master_update(c->knob) != 0;
    uint32_t key_down = c->keys & ~g_keys_before, key_up = g_keys_before & ~c->keys;
    uint32_t pressed = c->buttons & ~g_buttons_before, released = g_buttons_before & ~c->buttons;
    g_keys_before = c->keys;
    g_buttons_before = c->buttons;
    const uint32_t held = c->buttons;
    if (pressed & BUTTON(SEQ)) seq_used = 0;
    if (pressed & BUTTON(FX)) fx_used = 0;
    if (pressed & BUTTON(ARP)) arp_used = 0;
    if (pressed & BUTTON(GLO)) glo_used = 0;
    if (pressed & BUTTON(SAVE)) save_used = 0;
    if (pressed & BUTTON(HOME)) home_used = 0;
    for (unsigned key = 0; key < G_SLOTS; ++key) {
        if (!((key_down >> key) & 1U)) continue;
        int white = g_white_of(key);
        if ((held & BUTTON(SEQ)) && white >= 0) {
            static const char states[5][4] = {"OFF", "ON", "X2", "X3", "X4"};
            unsigned step = g_step_page * 16U + (unsigned)white, state = glitch_toggle_step(g_slot, step);
            char n[4] = {(char)('0' + (step + 1U) / 10U), (char)('0' + (step + 1U) % 10U), 0};
            strcpy(g_notice_text, "STEP "); strcat(g_notice_text, step + 1U < 10U ? n + 1 : n);
            strcat(g_notice_text, " "); strcat(g_notice_text, states[state < 5U ? state : 0]);
            g_set_notice(g_notice_text, now);
            seq_used = 1;
        } else if (held & BUTTON(FX)) {
            unsigned effect = g_key_effect(key);
            fx_used = 1;
            if (effect >= PUNCH) { punch_key = key; glitch_punch(effect - PUNCH); }
            else { fx_key = key; glitch_perform(effect); }
        } else if (held & BUTTON(GLO)) {
            glitch_toggle_mute(key); glo_used = 1;
        } else if ((held & BUTTON(SAVE)) && white >= 0) {
            save_used = 1;
            if (glitch_save((unsigned)white)) g_set_notice("SAVE FAILED", now);
            else g_pattern_notice("SAVED", (unsigned)white, now);
        } else if ((held & BUTTON(HOME)) && white >= 0) {
            home_used = 1;
            glitch_select_pattern((unsigned)white);
            g_pattern_notice(glitch_running() ? "NEXT BAR" : "LOADED", (unsigned)white, now);
        } else if (held & BUTTON(ARP)) {
            arp_used = 1;
            g_slot = key;
            glitch_randomize_slot(key, now * 2654435761U + key);
            glitch_hit(key);
            g_set_notice("NEW SLOT SOUND", now);
        } else {
            g_slot = key;
            glitch_hit(key);
        }
        changed = 1;
    }
    /* An effect ends with its key; another held key of the same kind (stutter
     * or punch-in) takes over. Both kinds can run at once. */
    if (fx_key < 27U && (((key_up >> fx_key) & 1U) || !(held & BUTTON(FX)))) {
        fx_key = 255;
        glitch_perform(GF_OFF);
        if (held & BUTTON(FX))
            for (unsigned key = 0; key < G_SLOTS; ++key) {
                unsigned effect = g_key_effect(key);
                if (effect != GF_OFF && effect < PUNCH && ((c->keys >> key) & 1U)) { fx_key = key; glitch_perform(effect); }
            }
        changed = 1;
    }
    if (punch_key < 27U && (((key_up >> punch_key) & 1U) || !(held & BUTTON(FX)))) {
        punch_key = 255;
        glitch_punch(GX_OFF);
        if (held & BUTTON(FX))
            for (unsigned key = 0; key < G_SLOTS; ++key) {
                unsigned effect = g_key_effect(key);
                if (effect >= PUNCH && ((c->keys >> key) & 1U)) { punch_key = key; glitch_punch(effect - PUNCH); }
            }
        changed = 1;
    }
    /* Buttons. */
    if ((pressed & BUTTON(PLAY)) && (held & BUTTON(SCL))) {
        glitch_tap(now);
        g_set_notice(glitch_bpm_external() ? "TEMPO FOLLOWS MIDI CLOCK" : "TAP TEMPO", now);
    } else if (pressed & BUTTON(PLAY)) glitch_transport(-1);
    if ((released & BUTTON(ARP)) && !arp_used) {
        static uint32_t rolls;
        glitch_randomize(now * 2654435761U + ++rolls);
        g_set_notice(glitch_running() ? "NEW BEAT NEXT BAR" : "NEW RANDOM BEAT", now);
    }
    if ((released & BUTTON(GLO)) && !glo_used && glitch_mutes()) {
        for (unsigned key = 0; key < G_SLOTS; ++key) if ((glitch_mutes() >> key) & 1U) glitch_toggle_mute(key);
        g_set_notice("ALL SLOTS ON", now);
    }
    if ((released & BUTTON(SAVE)) && !save_used) {
        if (glitch_save(glitch_pattern())) g_set_notice("SAVE FAILED", now);
        else g_pattern_notice("SAVED", glitch_pattern(), now);
    }
    if (pressed & BUTTON(REC)) { glitch_reset_loop(); g_set_notice("LOOP RESET", now); }
    if ((released & BUTTON(HOME)) && !home_used) g_page = PAGE_HOME;
    if ((released & BUTTON(FX)) && !fx_used) g_page = g_page >= PAGE_FILTER && g_page < PAGE_SYNC ? g_page + 1U : PAGE_FILTER;
    if ((released & BUTTON(SEQ)) && !seq_used) g_set_notice("HOLD SEQ + WHITE KEYS", now);
    if (pressed & BUTTON(EDIT)) g_page = g_page >= PAGE_SOUND && g_page < PAGE_REVERSE ? g_page + 1U : PAGE_SOUND;
    if (pressed & BUTTON(ENV)) g_page = PAGE_SOUND;
    if (pressed & BUTTON(LFO)) g_page = PAGE_FILTER;
    if ((pressed & BUTTON(SCL)) && !(held & BUTTON(PLAY))) g_page = PAGE_KIT;
    if (pressed & BUTTON(OCT_DOWN)) g_step_page = g_step_page ? g_step_page - 1U : 0U;
    if (pressed & BUTTON(OCT_UP)) g_step_page = g_step_page < 3U ? g_step_page + 1U : 3U;
    glitch_poll(now);
    /* Knobs: SELECT chooses the slot, PRESETS the pattern, K1-K4 the page. */
    if (c->encoders[0]) {
        int slot = (int)g_slot + c->encoders[0];
        g_slot = (unsigned)(slot < 0 ? 0 : slot >= G_SLOTS ? G_SLOTS - 1 : slot);
    }
    if (c->encoders[6]) {
        int next = glitch_pattern_next() >= 0 ? glitch_pattern_next() : (int)glitch_pattern();
        unsigned n = (unsigned)((next + c->encoders[6] % G_PATTERNS + G_PATTERNS) % G_PATTERNS);
        glitch_select_pattern(n);
        g_pattern_notice(glitch_running() ? "NEXT BAR" : "LOADED", n, now);
    }
    for (unsigned col = 0; col < 4; ++col) {
        int delta = c->encoders[2 + col];
        if (!delta) continue;
        unsigned entry = g_pages[g_page].p[col];
        if (entry == NONE) continue;
        if (entry >= GLOBAL) glitch_global_edit(entry - GLOBAL, delta);
        else glitch_slot_edit(g_slot, entry, delta);
    }
    changed |= pressed != 0 || released != 0 || encoder_active(c->encoders);
    if (g_notice && (int32_t)(now - g_notice_until) >= 0) { g_notice = NULL; changed = 1; }
    static uint32_t activity_at;
    if (now - activity_at >= 40U) { activity_at = now; g_activity_shown = glitch_activity(); }
    g_leds(c);
    if (saved_cache.shown_page == ~0U) g_full = 1; /* LCD transfer error: repaint all */
    if (changed || g_full || now - g_frame_ms >= 40U) {
        g_frame_ms = now;
        saved_cache.shown_page = 0;
        paint_priority = 0;
        cache = &saved_cache;
        if (!paint_rendering) {
            if (g_full) {
                paint_fill(0, 0, FM1_SCREEN_WIDTH, FM1_SCREEN_HEIGHT, BACK);
                g_full = 0; memset(g_sig, 0, sizeof g_sig); memset(cache, 0xff, sizeof *cache);
            }
            /* Repaint only the regions whose content changed. */
            char name[12];
            glitch_slot_name(g_slot, name);
            uint32_t s0 = signature(glitch_pattern() | (glitch_running() << 5) | ((uint32_t)(glitch_pattern_next() + 1) << 6) |
                                    (glitch_bpm() << 11) | (glitch_bpm_external() << 19) | (glitch_mutated() << 20) |
                                    ((glitch_global_get(GG_MUTATE) != 0) << 21) | (glitch_performing() << 22) | (g_slot << 27),
                                    name, (unsigned)strlen(name)) ^ glitch_punching() * 0x9e3779b9U ^ (glitch_slot_get(g_slot, GP_LENGTH) * 977U + glitch_slot_get(g_slot, GP_RATE));
            uint64_t st = glitch_steps(g_slot);
            uint32_t ratchets = 0;
            for (unsigned i = 0; i < G_STEPS; ++i) ratchets = ratchets * 31U + glitch_step_ratchet(g_slot, i);
            uint32_t s1 = (uint32_t)st * 3U ^ (uint32_t)(st >> 32) * 7U ^ (glitch_position(g_slot) << 24) ^ (g_step_page << 30) ^
                          glitch_slot_get(g_slot, GP_LENGTH) * 131U ^ glitch_running() * 0x5bd1e995U ^ ratchets * 2246822519U;
            uint32_t s2 = g_activity_shown * 2654435761U ^ glitch_mutes() * 40503U ^ g_slot;
            uint32_t s3 = signature(g_page | (g_cpu_load() << 5), g_notice ? g_notice : "", g_notice ? (unsigned)strlen(g_notice) : 0);
            g_dirty = 0;
            if (s0 != g_sig[0]) { g_sig[0] = s0; g_dirty |= 1U; paint_fill(0, 0, FM1_SCREEN_WIDTH, 46, BACK); }
            if (s1 != g_sig[1]) { g_sig[1] = s1; g_dirty |= 2U; paint_fill(0, 48, FM1_SCREEN_WIDTH, 62, BACK); }
            if (s2 != g_sig[2]) { g_sig[2] = s2; g_dirty |= 4U; paint_fill(0, 110, FM1_SCREEN_WIDTH, 44, BACK); }
            if (s3 != g_sig[3]) { g_sig[3] = s3; g_dirty |= 8U; paint_fill(0, 156, FM1_SCREEN_WIDTH, 36, BACK); }
        }
        g_draw();
    }
    paint_flush();
}
