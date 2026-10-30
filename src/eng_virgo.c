/* SPDX-License-Identifier: MIT
 * VIRGO: interferometer, binary coalescence and spectrum-driven FM.
 * A Felucca engine: two arms interfere; a note is a coalescence event
 * (inspiral -> merger -> ringdown); the output envelope feeds back into
 * the FM index (spectrum -> FM).
 *
 * MODE:   INTERF  two arms, static beat
 *         COALESCE one shot: inspiral, merger, ringdown
 *         DRONE   sustained interference
 *         DUST    interference + granular impulses
 * RATIO:  arm B interval above the key (semitones)
 * INDEX:  FM index / interference depth
 * DECAY:  ringdown time
 * TENSION:how fast the inspiral converges
 * DUST:   impulse density
 * COLOR:  output tilt (low-pass)
 * SPIN:   ringdown vibrato
 */
#include "felucca_engine.h"

static const char *const V_MODE[] = {"INTERF", "COALESCE", "DRONE", "DUST"};
enum { V_INTERF, V_COALESCE, V_DRONE, V_DUST };

#define VIR_INSPIRAL_BLOCKS(tens) FE_T(64 + (int32_t)(127 - (tens)) * 3)
#define VIR_MERGER_BLOCKS         FE_T(8)

static void virgo_note_on(track_t *t, voice_t *v)
{
    uint32_t seed = (v->note + 1u) * 0x9E3779B9u ^ (uint32_t)v->fine * 0x85EBCA6Bu;
    (void)t;
    v->s[0] = 0; v->s[1] = 0;
    v->s[2] = (int32_t)(seed ? seed : 1u);
    v->s[3] = 0; v->s[4] = 0; v->s[5] = 0; v->s[6] = 0; v->s[7] = 0;
    v->ph[0] = 0; v->ph[1] = 0; v->ph[2] = 0;
}

static uint32_t virgo_inc(int32_t pitch16)
{
    return pitch_inc((uint32_t)clamp(pitch16, 0, 2047));
}

static void virgo_render(track_t *t, voice_t *v, int32_t *out, uint32_t n, const vmod_t *m)
{
    const int16_t *p = t->p;
    uint32_t mode = (uint32_t)p[P_E0] & 3u, i;
    int32_t ratio = p[P_E1], index = p[P_E2], decay = p[P_E3];
    int32_t tension = p[P_E4], dust = p[P_E5], color = p[P_E6], spin = p[P_E7];
    int32_t a0 = v->s[0], a1 = v->s[1], rng = v->s[2], env = v->s[3];
    int32_t rd = v->s[4], lp = v->s[6], vib = v->s[7];
    uint32_t phc = v->ph[0], phm = v->ph[1], blocks = v->ph[2];
    int32_t T = VIR_INSPIRAL_BLOCKS(tension);
    int32_t idx, r16 = ratio, gain = 32767, cut, g;
    uint32_t incA, incB;

    if (mode == V_COALESCE) {
        if (blocks < (uint32_t)T) {
            int32_t x = (int32_t)(((int64_t)blocks << 16) / T);
            r16 = ratio * (32768 - (x >> 1)) >> 15;
            idx = (index * (16384 + (x >> 1))) >> 15;
            gain = 32767;
        } else if (blocks < (uint32_t)T + VIR_MERGER_BLOCKS) {
            r16 = 0; idx = index; gain = 32767;
        } else {
            r16 = 0; idx = index >> 1;
            rd = (rd * (65536 - ((decay + 8) << 9))) >> 16;
            if (rd < 0) rd = 0;
            gain = rd;
            vib += (uint32_t)((spin * 4096) >> 7);
        }
    } else {
        r16 = ratio; idx = index; gain = 32767;
    }

    incA = m->inc;
    incB = virgo_inc(m->pitch16 + r16 * 16);
    idx = (idx * (16384 + (env >> 1))) >> 15;
    idx = clamp(idx, 0, 127);

    for (i = 0; i < n; i++) {
        int32_t armA, armB, diff, fm, y, w = 0;
        armA = SINE[a0 >> 20];
        armB = SINE[a1 >> 20];
        a0 += incA; a1 += incB;
        diff = (armA - armB) >> 1;
        y = ((armA + armB) >> 1) + ((diff * idx) >> 7);
        fm = SINE[phm >> 20];
        phc += incA + (((fm * idx) >> 7) * 64);
        phm += incB;
        y = (y >> 1) + (SINE[phc >> 20] >> 1);
        if (dust && mode == V_DUST) {
            uint32_t r = noise32(&rng);
            if ((int32_t)(r & 0xFFFFu) < (dust * 200))
                w += (r & 0x20000u) ? 8192 : -8192;
        }
        y += w;
        cut = clamp((127 - color) << 8, 0, 127 << 8);
        g = SVF_G[cut >> 8];
        lp += ((y - lp) * g) >> 12;
        y = lp;
        y = (y * gain) >> 15;
        env += ((clamp(y, -16384, 16384) - env) >> 9);
        out[i] += voice_amp(y, m, i);
    }

    blocks++;
    if (mode == V_COALESCE && blocks > (uint32_t)T + VIR_MERGER_BLOCKS + FE_T(8 + decay * 3))
        blocks = 0, rd = 0;

    v->s[0] = a0; v->s[1] = a1; v->s[2] = rng; v->s[3] = env;
    v->s[4] = rd; v->s[6] = lp; v->s[7] = vib;
    v->ph[0] = phc; v->ph[1] = phm; v->ph[2] = blocks;
}

static const preset_t VIRGO_PRESETS[] = {
    {"BINARY", {V_INTERF, 7, 40, 60, 64, 0, 40, 0}, {60, 90, 110, 90}, 0, 0, FX(0, 30, 20, 80), PAT(5)},
    {"CHIRP",  {V_COALESCE, 12, 90, 70, 100, 20, 50, 30}, {10, 100, 127, 60}, 0, 0, FX(0, 0, 30, 90), PAT(3)},
    {"RING",   {V_COALESCE, 5, 60, 110, 40, 0, 70, 60}, {0, 80, 90, 100}, 0, 0, FX(0, 20, 40, 100), PAT(7)},
    {"DUST",   {V_DUST, 3, 50, 80, 64, 100, 30, 0}, {20, 70, 100, 80}, 0, 0, FX(0, 0, 20, 70), PAT(5)},
};

static const engine_t ENG_VIRGO = {
    .name = "VIRGO",
    .page_title = {"ARMS", "EVENT"},
    .edit = {
        {"MODE",    F_ENUM,   0, 3,   1, V_MODE, 0},
        {"RATIO",   F_SEMI, -24, 24,  7, 0, "st"},
        {"INDEX",   F_PCT,    0, 127, 50, 0, 0},
        {"DECAY",   F_PCT,    0, 127, 70, 0, 0},
        {"TENSION", F_PCT,    0, 127, 64, 0, 0},
        {"DUST",    F_PCT,    0, 127, 20, 0, 0},
        {"COLOR",   F_PCT,    0, 127, 40, 0, 0},
        {"SPIN",    F_PCT,    0, 127, 20, 0, 0},
    },
    .presets = VIRGO_PRESETS,
    .npresets = NELEM(VIRGO_PRESETS),
    .note_on = virgo_note_on,
    .render = virgo_render,
    .knob = {P_E2, P_E4, P_E1, P_REL},
    .keep = 0x00,
    .oneshot = 0,
};
