/* SPDX-License-Identifier: GPL-2.0-or-later */
/* dsl_stretch: time stretch and pitch for one DigiSlicer voice, in integers only (the MCF54418 has
 * no FPU). Written to run unchanged in the render (ev_render_voices), 32 samples a block.
 *
 * Read heads over the sample's own PCM and a stretch grid:
 *   - the grid moves through the slice at STRETCH (its rate sets the slice's length);
 *   - the live head plays at PITCH;
 *   - when the head has drifted GRAIN samples off the grid, a new head takes over near the
 *     grid, placed so that its dominant peak meets the old head's (peak alignment), over a
 *     smoothstep crossfade;
 *   - onsets inside the slice are never played early or twice: a head that would reach the next
 *     onset before the grid splices back, and the grid reaching an onset punches a head onto it.
 * A crossfade that starts during another fades the outgoing pair out together (three heads at
 * most), so a trig or punch never cuts a half-faded head.
 * With STRETCH equal to PITCH the head never leaves the grid: plain repitched playback.
 *
 * The idea of extremum-aligned splices and transient punches comes from Heavylight Industries'
 * capicola ("Keyframe Time Stretching via Extrema Sampling", DAFx26). This code is our own and
 * shares none of capicola's. */
#ifndef DSL_STRETCH_H
#define DSL_STRETCH_H

#include <stdint.h>

#define DSL_HEADS 3

typedef struct {
    int32_t i;                          /* sample index */
    uint32_t f;                         /* fraction, 32 bits */
} dsl_pos;

typedef struct {
    dsl_pos p;
    const int16_t *pcm;                 /* its sample (a trig may change the voice's) */
    int32_t len;
    int32_t w;                          /* outgoing heads: weight in the outgoing mix, Q15; 0 idle */
} dsl_head;

typedef struct {
    const int16_t *pcm;                 /* the slice's sample */
    int32_t len;
    int32_t a, b;                       /* the slice: [a, b) */
    const int32_t *on;                  /* onsets, sorted; only those in (a, b) are used */
    int32_t non;
    int32_t next;                       /* the first onset the grid has not reached */
    int32_t floor;                      /* no head starts before it: the slice start or last punch */
    dsl_pos grid;
    dsl_head head[DSL_HEADS];
    int32_t live;                       /* the head followed */
    int32_t fading;
    uint32_t fph, fstep;                /* fade phase, Q16 (65536: done), and its step */
    uint32_t fleft;                     /* samples left in the fade */
    int32_t tail;                       /* > 0: fading out at the slice's end */
    int32_t active;

    /* settings (dsl_config) */
    uint32_t rs, rp;                    /* STRETCH and PITCH rates, Q16.16 */
    int32_t grain;                      /* head-to-grid drift that starts a splice, samples */
    int32_t half;                       /* peak search half-width, samples; 0: no alignment */
    int32_t fade;                       /* splice fade, output samples (from the rates) */
    int32_t reach;                      /* source samples a head reads during a fade, + margin */
    int32_t punch_fade;                 /* trig and onset fade, output samples */
    int32_t gsh, hsh, dsh;              /* log2 bounds on the grid's, a head's, their distance's step */
    int32_t thresh;                     /* the drift that starts the next splice */
    int32_t stagger;                    /* ... less this after a trig or punch: voices that start
                                           together splice apart (set by the caller) */
    int32_t *budget;                    /* splice searches left this block, shared by voices; the
                                           caller refills it; 0 = unlimited. A drift splice waits
                                           for it; a guard does not. */

    /* counters, for the host */
    uint32_t n_splice, n_guard, n_punch, n_three;
} dsl_voice;

/* Rates are Q16.16 (65536 = 1.0): rs the grid's, rp the heads'. */
void dsl_config(dsl_voice *v, uint32_t rs, uint32_t rp, int32_t grain, int32_t half);

/* A trig: play the slice [a, b) of pcm, crossfading from whatever played. on[] are the sample's
 * onsets (sorted); the ones inside the slice are punched. */
void dsl_trigger(dsl_voice *v, const int16_t *pcm, int32_t len, int32_t a, int32_t b,
                 const int32_t *on, int32_t non);

/* n samples (16-bit scale, in 32 bits) into out. */
void dsl_render(dsl_voice *v, int32_t *out, int32_t n);

#endif
