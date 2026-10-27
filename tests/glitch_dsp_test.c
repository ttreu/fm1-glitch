/* Native unit test of the GLITCH audio path: every sound source at the
 * extremes of its knobs, the polymetric sequencer, and every momentary
 * effect, all with the real master effects. */
#include "../app/glitch_dsp.c"
#include <math.h>
#include <stdio.h>
#include <string.h>

static int fails;
static void check(int ok, const char *msg) { printf("%s %s\n", ok ? "PASS" : "FAIL", msg); if (!ok) ++fails; }
static int32_t out[2 * BLOCK];
static void patch(float bpm, float rev_mix, float tape_fb)
{
    struct fx_patch *p = &fx_patches[0];
    memset(p, 0, sizeof *p);
    p->bpm = bpm; p->cutoff = 100.0f; p->drive = 1.0f;
    p->tape_time = 0.3f * FX_RATE; p->tape_feedback = tape_fb; p->tape_tone = 0.3f; p->tape_mix = 0.3f; p->tape_wow = 0.3f;
    p->delay_time = 0.25f * FX_RATE; p->delay_feedback = 0.5f; p->delay_mix = 0.2f;
    p->rev_decay = 0.9f; p->rev_damp = 0.3f; p->rev_shimmer = 0.5f; p->rev_mix = rev_mix; p->rev_dry = 1.0f;
    fx_patch_index = 0;
}
/* Render `blocks`; returns the peak, 0 if anything was not finite. */
static double render(unsigned blocks, double *energy)
{
    double peak = 0.0, e = 0.0;
    for (unsigned b = 0; b < blocks; ++b) {
        memset(out, 0, sizeof out);
        glitch_render(out);
        for (unsigned i = 0; i < 2 * BLOCK; ++i) {
            double v = out[i] / 8388607.0;
            if (!isfinite(v)) return -1.0;
            if (fabs(v) > peak) peak = fabs(v);
            e += v * v;
        }
    }
    if (energy) *energy = e;
    return peak;
}
static void slot(unsigned s, unsigned source, unsigned pitch, unsigned decay, unsigned color)
{
    memset(&g_live[s], 0, sizeof g_live[s]);
    uint8_t *v = g_live[s].value;
    v[GP_SOURCE] = (uint8_t)source; v[GP_PITCH] = (uint8_t)pitch; v[GP_DECAY] = (uint8_t)decay; v[GP_COLOR] = (uint8_t)color;
    v[GP_LEVEL] = 99; v[GP_PROB] = 100; v[GP_LENGTH] = 16; v[GP_RANDOM] = 50; v[GP_PAN] = 50; v[GP_TAPE] = 50; v[GP_SHIMMER] = 50;
}
int main(void)
{
    static const char *names[GS_COUNT] = {"HAT", "IMPULSE", "TRAIN", "CRUNCH", "TICK", "CHIRP", "ZAP", "SNAP", "DUST", "BITS", "GRAIN", "THUMP", "POP", "LOOP", "HIGH", "DROP"};
    patch(120.0f, 0.5f, 0.6f);
    g_level = 0.8f;
    /* Every source at the corners of PITCH / DECAY / COLOR. */
    for (unsigned src = 0; src < GS_COUNT; ++src) {
        int ok = 1, sounded = 0;
        for (unsigned c = 0; c < 8; ++c) {
            slot(0, src, c & 1 ? 48 : 0, c & 2 ? 99 : 0, c & 4 ? 99 : 0);
            glitch_hit(0);
            double e = 0.0, peak = render(200, &e);
            if (peak < 0.0 || peak > 1.0) ok = 0;
            if (e > 1e-6) sounded = 1;
        }
        char msg[64];
        snprintf(msg, sizeof msg, "%s: finite and bounded at all knob corners%s", names[src], src == GS_DROP ? "" : ", audible");
        check(ok && (sounded || src == GS_DROP), msg);
    }
    /* Polymeter: lengths 16, 12 and 5 at 1/16; after 2 bars they sit at 0, 8, 2. */
    memset(g_live, 0, sizeof g_live);
    slot(1, GS_IMPULSE, 24, 10, 0); g_live[1].value[GP_LENGTH] = 16;
    slot(2, GS_TICK, 24, 10, 0);    g_live[2].value[GP_LENGTH] = 12;
    slot(3, GS_CRUNCH, 24, 10, 0);    g_live[3].value[GP_LENGTH] = 5;
    g_live[1].steps = g_live[2].steps = g_live[3].steps = 1;
    g_run = 1;
    patch(120.0f, 0.0f, 0.0f);
    /* 2 bars at 120 BPM = 4 s; stop just before the 33rd step. */
    unsigned blocks = (unsigned)(4.0 * FX_RATE / BLOCK) - 4U;
    render(blocks, NULL);
    printf("     positions after 2 bars: %u %u %u\n", g_position[1], g_position[2], g_position[3]);
    check(g_position[1] == 15 && g_position[2] == 7 && g_position[3] == 1, "each slot loops through its own length");
    /* Every momentary effect on a running dense beat. */
    for (unsigned s = 4; s < 20; ++s) { slot(s, 1 + s % 10, 24, 30, 50); g_live[s].steps = 0x5555U << (s & 1); }
    patch(120.0f, 0.6f, 0.9f);
    for (unsigned fx = 1; fx < GF_COUNT; ++fx) {
        g_perform_now = (uint8_t)fx;
        double a = render(400, NULL);
        g_perform_now = GF_OFF;
        double b = render(100, NULL);
        char msg[64];
        snprintf(msg, sizeof msg, "effect %u: finite, bounded, ends cleanly (peak %.2f)", fx, a);
        check(a >= 0.0 && a <= 1.0 && b >= 0.0 && active_fx == GF_OFF, msg);
    }
    /* A pattern swap waits for the bar start. */
    for (unsigned s = 0; s < G_SLOTS; ++s) g_staged[s] = g_live[s];
    g_staged[1].steps = 0xffff;
    g_staged_ready = 1;
    render(4, NULL);
    check(g_staged_ready == 1, "a staged pattern waits for the next bar");
    render((unsigned)(2.1 * FX_RATE / BLOCK), NULL);
    check(g_staged_ready == 0 && g_live[1].steps == 0xffff, "it takes over at the bar start");
    /* REVERSE: every source stays finite; all but the impulse swell up
     * instead of fading (more energy in the second half of the sound). */
    for (unsigned src = GS_HAT; src < GS_DROP; ++src) {
        double half[2][2] = {{0}};
        int ok = 1;
        for (unsigned rev = 0; rev < 2; ++rev) {
            slot(0, src, 24, 60, 50);
            g_live[0].value[GP_RANDOM] = 0;
            g_live[0].value[GP_REVERSE] = rev ? 100 : 0;
            memset(voices, 0, sizeof voices);
            trigger(0, 127, 0);
            static float env[3 * 44100];
            unsigned n = 0;
            while (voices[0].active && n < 3U * 44100U) {
                float y = voice_sample(&voices[0]);
                if (!isfinite(y) || fabsf(y) > 4.0f) ok = 0;
                env[n++] = y;
            }
            for (unsigned i = 0; i < n; ++i) half[rev][i * 2U >= n] += (double)env[i] * env[i];
        }
        char msg[80];
        if (src == GS_IMPULSE) snprintf(msg, sizeof msg, "%s reversed: finite", names[src]);
        else snprintf(msg, sizeof msg, "%s reversed: swells (%.2f -> %.2f), forward fades (%.2f -> %.2f)", names[src],
                      half[1][0], half[1][1], half[0][0], half[0][1]);
        check(ok && (src == GS_IMPULSE || (half[1][1] > half[1][0] && half[1][0] + half[1][1] > 1e-4)), msg);
    }
    /* Step ratchets: the step's own ratchet wins over the slot's RATCHET. */
    {
        static const struct { uint8_t step, slot_ratchet, hits; const char *what; } cases[] = {
            {0, 0, 1, "a plain step plays once"}, {2, 0, 3, "step ratchet X3 plays 3 hits"},
            {0, 1, 2, "slot RATCHET X2 still works"}, {3, 1, 4, "step ratchet X4 beats slot RATCHET X2"}};
        for (unsigned c = 0; c < 4; ++c) {
            memset(g_live, 0, sizeof g_live);
            slot(1, GS_IMPULSE, 24, 10, 0);
            g_live[1].steps = 1;
            g_live[1].ratchet[0] = cases[c].step;
            g_live[1].value[GP_RATCHET] = cases[c].slot_ratchet;
            g_mutes = 0; g_staged_ready = 0;
            event_count = 0; tick = 0;
            for (unsigned s = 0; s < G_SLOTS; ++s) pos[s] = sub[s] = 0;
            on_tick(now, 1000.0f);
            unsigned hits = 0;
            for (unsigned i = 0; i < event_count; ++i) hits += events[i].slot == 1;
            char msg[80];
            snprintf(msg, sizeof msg, "%s (%u)", cases[c].what, hits);
            check(hits == cases[c].hits, msg);
        }
    }
    /* Punch-in sequencer effects (FX + black key). */
    {
        static const struct { uint8_t punch, step, hits; const char *what; } cases[] = {
            {GX_RATCHET_3, 0, 3, "punch RATCHET X3 turns a plain step into 3 hits"},
            {GX_RATCHET_2, 3, 4, "punch RATCHET X2 keeps a step's own X4"},
            {GX_ROLL, 0, 5, "punch ROLL plays 5 hits"}};
        for (unsigned c = 0; c < 3; ++c) {
            memset(g_live, 0, sizeof g_live);
            slot(1, GS_IMPULSE, 24, 10, 0);
            g_live[1].steps = 1;
            g_live[1].ratchet[0] = cases[c].step;
            g_punch_now = cases[c].punch;
            event_count = 0; tick = 0;
            for (unsigned s = 0; s < G_SLOTS; ++s) pos[s] = sub[s] = 0;
            on_tick(now, 1000.0f);
            unsigned hits = 0, rising = 1, last = 0;
            for (unsigned i = 0; i < event_count; ++i)
                if (events[i].slot == 1) { ++hits; if (events[i].velocity < last) rising = 0; last = events[i].velocity; }
            char msg[96];
            snprintf(msg, sizeof msg, "%s (%u)", cases[c].what, hits);
            check(hits == cases[c].hits && (cases[c].punch != GX_ROLL || rising), msg);
        }
        slot(0, GS_ZAP, 24, 50, 50);
        g_punch_now = GX_REVERSE;
        memset(voices, 0, sizeof voices);
        trigger(0, 127, 0);
        check(voices[0].active && voices[0].reverse, "punch REVERSE ALL plays a slot without REVERSE backwards");
        g_punch_now = GX_CHAOS;
        unsigned backwards_hits = 0;
        for (unsigned i = 0; i < 40; ++i) { memset(voices, 0, sizeof voices); trigger(0, 127, 0); backwards_hits += voices[0].reverse; }
        check(backwards_hits > 5 && backwards_hits < 35, "punch CHAOS plays some hits backwards");
        /* A dense beat under every punch effect stays finite. */
        for (unsigned s = 4; s < 20; ++s) { slot(s, 1 + s % 10, 24, 30, 50); g_live[s].steps = 0x5555U << (s & 1); }
        g_run = 1;
        int ok = 1;
        for (unsigned px = 1; px < GX_COUNT; ++px) { g_punch_now = (uint8_t)px; double a = render(300, NULL); if (a < 0.0 || a > 1.0) ok = 0; }
        g_punch_now = GX_OFF;
        check(ok, "every punch effect on a dense beat: finite and bounded");
    }
    /* ACCENT and FLAM: downbeats louder; some hits come twice, between the
     * steps, and the same ones again in the next bar. */
    {
        memset(g_live, 0, sizeof g_live);
        slot(1, GS_IMPULSE, 24, 10, 0);
        g_live[1].steps = 0xffff;
        g_punch_now = GX_OFF; g_accent = 0.8f; g_flam = 1.0f; g_seed = 7U;
        unsigned bar_hits[2] = {0}, between_hits = 0, loud = 0, soft_ = 0;
        uint32_t first_bar[64]; unsigned n_first = 0, same = 0;
        for (unsigned b = 0; b < 2; ++b) {
            for (unsigned s = 0; s < G_SLOTS; ++s) pos[s] = sub[s] = 0;
            for (unsigned t = 0; t < 64; ++t) {
                event_count = 0; tick = t;
                uint32_t base = 1000000U + t * 1000U;
                on_tick(base, 1000.0f);
                for (unsigned i = 0; i < event_count; ++i) {
                    uint32_t at = events[i].time - base;
                    ++bar_hits[b];
                    if (at > 100U) {
                        ++between_hits;
                        if (b == 0 && n_first < 64) first_bar[n_first++] = t * 1000U + at;
                        else for (unsigned k = 0; k < n_first; ++k) same += first_bar[k] == t * 1000U + at;
                    }
                    if (t == 0) loud = events[i].velocity;
                    if (t == 4 && at <= 100U) soft_ = events[i].velocity;
                }
            }
        }
        char msg[96];
        snprintf(msg, sizeof msg, "FLAM: %u extra hits between the steps, %u repeat in bar 2", between_hits, same);
        check(between_hits >= 4 && bar_hits[1] > 16 && same * 2 == between_hits, msg);
        snprintf(msg, sizeof msg, "ACCENT: downbeat %u louder than the next sixteenth %u", loud, soft_);
        check(loud > soft_ + 30U, msg);
        g_flam = 0.0f; g_accent = 0.0f;
    }
    /* PAN JUMP: some hits go all the way to one side, past echo and reverb. */
    {
        patch(120.0f, 0.6f, 0.5f);
        g_run = 0; g_level = 0.6f;
        memset(g_live, 0, sizeof g_live);
        slot(0, GS_SNAP, 24, 40, 50);
        g_live[0].value[GP_PAN_MODE] = PAN_JUMP; g_live[0].value[GP_PAN] = 99;
        memset(voices, 0, sizeof voices);
        unsigned one_side = 0, middle = 0, leaks = 0;
        for (unsigned h = 0; h < 12; ++h) {
            for (unsigned b = 0; b < 3000; ++b) { memset(out, 0, sizeof out); glitch_render(out); }
            glitch_hit(0);
            double l = 0.0, r = 0.0;
            for (unsigned b = 0; b < 30; ++b) {
                memset(out, 0, sizeof out); glitch_render(out);
                for (unsigned i = 0; i < BLOCK; ++i) { l = fmax(l, fabs(out[2 * i] / 8388607.0)); r = fmax(r, fabs(out[2 * i + 1] / 8388607.0)); }
            }
            double q = l < r ? l / (r + 1e-12) : r / (l + 1e-12);
            if (q < 1e-3) ++one_side; else if (q > 0.5) ++middle; else ++leaks; /* -60 dB: older reverb tails */
        }
        char msg[96];
        snprintf(msg, sizeof msg, "PAN JUMP: %u hits 100/0, %u in the middle, %u in between", one_side, middle, leaks);
        check(one_side >= 2 && middle >= 2 && leaks == 0, msg);
    }
    return fails ? 1 : 0;
}
