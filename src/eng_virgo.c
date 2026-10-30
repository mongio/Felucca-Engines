/* SPDX-License-Identifier: MIT
 * VIRGO: interferometer, binary coalescence and spectrum-driven FM.
 * Felucca engine. Two arms (sines) interfere; the carrier is phase-modulated by arm B.
 * In COALESCE a note is one event: inspiral (arms converge, level and FM index rise),
 * merger (unison, full index), ringdown (exponential decay). The output level feeds back
 * into the FM index (spectrum -> FM).
 *
 * MODE    INTERF   static interference of two arms
 *         COALESCE one event per note: inspiral, merger, ringdown
 *         DRONE    interference with a slow drift of arm B
 *         DUST     interference plus random impulses
 * RATIO   arm B above arm A, semitones
 * INDEX   FM depth
 * DECAY   ringdown time (tau 0.2 .. 5.5 s)
 * TENSION how fast the inspiral runs (127 = 1 s, 0 = 10 s)
 * DUST    impulse density
 * COLOR   brightness (0 = open, 127 = dark)
 * SPIN    ringdown detune of arm B
 *
 * State: s[3] output level follower, s[4] ringdown level (Q24), s[5] ringdown started,
 * s[6] filter, s[7] drift phase (uint32), s[2] random generator;
 * ph[0] arm A, ph[1] arm B, ph[2] block counter. */
static const char *const VG_MODE[] = {"INTERF", "COALESCE", "DRONE", "DUST"};
enum { VG_INTERF, VG_COALESCE, VG_DRONE, VG_DUST };

#define VG_INSPIRAL(tens) FE_T(1400 + (127 - (tens)) * 100)   /* blocks: 1 .. 10 s */
#define VG_MERGER FE_T(150)                                   /* blocks: about 0.1 s */
#define VG_RING(decay) FE_T(300 + (decay) * 60)               /* blocks: tau 0.2 .. 5.5 s */

static void virgo_note_on(track_t *t, voice_t *v)
{
    uint32_t seed = (v->note + 1u) * 0x9E3779B9u ^ (uint32_t)v->fine * 0x85EBCA6Bu;
    (void)t;
    v->s[0] = v->s[1] = 0;
    v->s[2] = (int32_t)(seed ? seed : 1u);
    v->s[3] = v->s[4] = v->s[5] = v->s[6] = v->s[7] = 0;
    v->ph[0] = v->ph[1] = v->ph[2] = 0;
}

static void virgo_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t mode = (uint32_t)p[P_E0] & 3u, i;
    int32_t ratio = p[P_E1], index = p[P_E2], decay = p[P_E3], tension = p[P_E4];
    int32_t dust = p[P_E5], color = p[P_E6], spin = p[P_E7];
    int32_t rng = v->s[2], env = v->s[3], rd = v->s[4], ringing = v->s[5], lp = v->s[6];
    uint32_t drift = (uint32_t)v->s[7];
    uint32_t a0 = v->ph[0], a1 = v->ph[1], blocks = v->ph[2];
    int32_t T = VG_INSPIRAL(tension), M = VG_MERGER;
    int32_t d16 = ratio * 16, idx = index, gain = 32767, kq;
    uint32_t incA, incB;

    if (mode == VG_COALESCE) {
        if ((int32_t)blocks < T) {                         /* inspiral */
            uint32_t x = ((uint32_t)blocks << 16) / (uint32_t)T;      /* 0 .. 65535 */
            uint32_t xx = (x * x) >> 16;                              /* x squared */
            d16 = (ratio * 16 * (65536 - (int32_t)xx)) >> 16;         /* the arms converge */
            idx = (index * (32768 + (int32_t)(x >> 1))) >> 15;        /* the index rises */
            gain = 12000 + (((32767 - 12000) * (int32_t)x) >> 16);    /* so does the level */
        } else if ((int32_t)blocks < T + M) {              /* merger: unison, full index */
            d16 = 0;
        } else {                                           /* ringdown */
            if (!ringing) {
                ringing = 1;
                rd = 1 << 24;
            }
            d16 = spin >> 3;
            idx = index >> 1;
            rd -= rd / VG_RING(decay);
            gain = rd >> 9;
        }
    } else if (mode == VG_DRONE) {
        d16 += sine_i(drift) >> 13;                        /* a slow wander of arm B */
        drift += 0x00080000u;
    }

    incA = m->inc;
    incB = pitch_inc((uint32_t)clamp(m->pitch16 + d16, 0, 2047));
    idx = clamp(idx + (((env >> 8) * index) >> 7), 0, 127);   /* spectrum -> FM: loud makes bright */
    kq = clamp(1200 + (127 - color) * (127 - color) * 2, 1200, 32767);

    for (i = 0; i < n; i++) {
        int32_t armA = sine_i(a0), armB = sine_i(a1), car, y, a;
        car = sine_i(a0 + (uint32_t)shl(armB * idx, 10));   /* arm B phase-modulates the carrier */
        a0 += incA;
        a1 += incB;
        y = (car + ((armA + armB) >> 1)) >> 1;               /* the carrier plus the beating arms */
        if (dust && mode == VG_DUST) {
            uint32_t r = noise32(&rng);
            if ((int32_t)(r & 0xFFFFu) < dust * 24)
                y += (r & 0x20000u) ? 8192 : -8192;
        }
        y = clamp(y, -32767, 32767);
        lp += (((y - lp) >> 3) * kq) >> 12;
        y = mulq15(lp, gain);
        a = y < 0 ? -y : y;
        env += (a - env) >> 9;
        out[i] += voice_amp(y, m, i);
    }

    if (blocks < 0x7FFFFFFFu)
        blocks++;
    v->s[2] = rng;
    v->s[3] = env;
    v->s[4] = rd;
    v->s[5] = ringing;
    v->s[6] = lp;
    v->s[7] = (int32_t)drift;
    v->ph[0] = a0;
    v->ph[1] = a1;
    v->ph[2] = blocks;
}

/* {MODE, RATIO, INDEX, DECAY, TENSION, DUST, COLOR, SPIN} */
static const preset_t VIRGO_PRESETS[] = {
    {"BINARY", {VG_INTERF, 7, 40, 60, 64, 0, 40, 0}, {60, 90, 110, 90}, 0, 0, FX(0, 30, 20, 80), PAT(5)},
    {"CHIRP", {VG_COALESCE, 12, 90, 70, 100, 20, 50, 30}, {10, 100, 127, 60}, 0, 0, FX(0, 0, 30, 90), PAT(3)},
    {"RING", {VG_COALESCE, 5, 60, 110, 40, 0, 70, 60}, {0, 80, 127, 100}, 0, 0, FX(0, 20, 40, 100), PAT(7)},
    {"DRIFT", {VG_DRONE, 7, 50, 80, 64, 0, 50, 0}, {40, 80, 127, 90}, 0, 0, FX(0, 30, 30, 90), PAT(5)},
    {"DUST", {VG_DUST, 3, 50, 80, 64, 100, 30, 0}, {20, 70, 100, 80}, 0, 0, FX(0, 0, 20, 70), PAT(5)},
};

static const engine_t ENG_VIRGO = {
    .name = "VIRGO",
    .page_title = {"ARMS", "EVENT"},
    .edit = {
        {"MODE", F_ENUM, 0, 3, 1, VG_MODE, 0},
        {"RATIO", F_SEMI, -24, 24, 7, 0, "st"},
        {"INDEX", F_PCT, 0, 127, 50, 0, 0},
        {"DECAY", F_PCT, 0, 127, 70, 0, 0},
        {"TENSION", F_PCT, 0, 127, 64, 0, 0},
        {"DUST", F_PCT, 0, 127, 20, 0, 0},
        {"COLOR", F_PCT, 0, 127, 40, 0, 0},
        {"SPIN", F_PCT, 0, 127, 20, 0, 0},
    },
    .presets = VIRGO_PRESETS,
    .npresets = NELEM(VIRGO_PRESETS),
    .note_on = virgo_note_on,
    .render = virgo_render,
    .knob = {P_E2, P_E4, P_E1, P_REL},
    .keep = 0x00,
};
