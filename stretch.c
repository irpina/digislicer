/* SPDX-License-Identifier: GPL-2.0-or-later */
/* stretch.c: time stretch on DIGISLICER tracks (dsl_stretch.c), a prototype.
 *
 * Route (a): the stock voice plays the slice as ever (its window, its DMA, the DSP's resampler), and
 * at machine-pages' ev_render_voices, order 50 (after playback, before NEIGHBOR copies a track at
 * 60), a stretched track's block is replaced with dsl_stretch's: the same slice, read from the
 * sample's PCM at the voice's pitch, its length set by STRETCH. The filter, AMP, LFOs and sends
 * then work on it as on the stock block.
 *
 *   - slice_win (glue.s) hands a stretched voice's window to ts_win every block;
 *   - on the block a voice starts (fw_voice_start), ts_render triggers dsl_stretch on that window,
 *     with the slot's slice starts (slc_pts: the sample's own slices or AUTO's) as its onsets, so
 *     a window of several slices (LEN > 0) punches each one in time;
 *   - from then on the track's block is ours until its next start, silence once the stretched
 *     slice is over, so a faster slice does not let the stock voice's tail through.
 *
 * STRETCH is knob G of DIGISLICER's SRC page, in GRID's slot (glue.s dsl_page): the render reads
 * it with the other parameters, after locks and LFOs (fw_voice_params[53 t + 6], 0-127). 0-7 is
 * OFF (so a kit saved with GRID 0-5 plays unstretched); from 8 it is the slice's speed, x0.25 at 8
 * to x1.00 at 72 and x2.00 at 104, an octave every 32 (x0.50 plays it twice as long, at its pitch).
 * Reverse PLAY plays stock.
 *
 * The render's budget: a splice's peak search is the costly moment, so at most TS_SEARCHES run a
 * block across the eight tracks (a drift splice waits a block for one), and each track's first
 * splice after a trig comes at a different drift (stagger), so tracks started together do not all
 * splice in the same block. */
#pragma GCC optimize ("O2")
#include "digitakt-mk1/core3.h"
#include "dsl_stretch.h"
#ifdef OS154
#include "os154.h"
#else
#include "os153.h"
#endif

#define DSL_ID      5
#define MAXSL       64
#define V(t)        (OS_VOICE_SLOT - 0x5C + 94u * (uint32_t)(t))   /* a voice's render state */
#define V_PCM(t)    (*(const int16_t *const volatile *)(V(t)))     /* +0  the slot's PCM */
#define V_RATE(t)   (*(const volatile uint32_t *)(V(t) + 0x0C))     /* +12 the playback rate, Q27 */
#define V_GAIN(t)   (*(const volatile int32_t *)(V(t) + 0x10))      /* +16 velocity x level, Q31 */
#define V_LEN(t)    (*(const volatile int32_t *)(V(t) + 0x14))      /* +20 the sample's length */
#define V_SLOT(t)   (*(const volatile uint8_t *)(V(t) + 0x5C))

extern volatile uint8_t slc_n[];
extern uint32_t (*slc_pts)[MAXSL];

volatile uint32_t dsl_ts_rate[8];       /* STRETCH as the render last took it, Q16.16; 0 off */
volatile int32_t dsl_ts_grain = 2048;   /* the splice drift, samples */
volatile int32_t dsl_ts_half = 384;     /* the splice peak search, samples */
volatile uint32_t dsl_ts_blocks;        /* blocks rendered, for a test */

static dsl_voice ts_v[8];
static uint32_t ts_lo[8], ts_hi[8];     /* the window slice_win gave this block */
static uint8_t ts_have[8];              /* ... if it did */
static uint8_t ts_own[8];               /* the track's block is ours until its next start */
static uint32_t ts_rs[8], ts_rp[8];     /* the rates dsl_config last had */
static int32_t ts_out[32];
static int32_t ts_budget;               /* splice searches left this block */
#define TS_SEARCHES 2
/* At most this many tracks stretch at once: a note that would make one more plays stock. In the
 * emulator's cycle model eight stretched voices at x0.25 reach 100.8% of the render's period (stock
 * 63%), and the unit has less headroom than the model. */
#define TS_MAX_VOICES 4

/* STRETCH's value (0-127) as a speed, Q16.16; 0 = OFF. 2^((v - 72) / 32) through the pitch table,
 * 2^((i - 10752) / 2048) in Q29. */
static uint32_t ts_speed(int32_t v)
{
    if (v < 8)
        return 0;
    if (v > 127)
        v = 127;
    return fw_pitch_tab[10752 + (v - 72) * 64] >> 13;
}

volatile uint32_t dsl_ts_force[8];      /* a test's STRETCH (Q16.16) in place of knob G; 0 none */

/* Track t's STRETCH, from the render's parameters. */
static uint32_t ts_rate(int32_t t)
{
    if (dsl_ts_force[t])
        return dsl_ts_force[t];
    return ts_speed((fw_voice_params[53 * t + 6] >> 8) & 0xff);
}

/* Knob G's text (machine-pages): OFF, or the speed, x0.25 ... x3.29. */
int32_t ts_fmt(char *buf, int32_t value, int32_t ctx, int32_t machine)
{
    uint32_t c = ts_speed((value >> 8) & 0xff);
    (void)ctx;
    (void)machine;
    if (!c) {
        buf[0] = 'O'; buf[1] = 'F'; buf[2] = 'F'; buf[3] = 0;
        return 1;
    }
    c = (c * 100 + 32768) >> 16;               /* hundredths */
    buf[0] = 'x';
    buf[1] = (char)('0' + c / 100 % 10);
    buf[2] = '.';
    buf[3] = (char)('0' + c / 10 % 10);
    buf[4] = (char)('0' + c % 10);
    buf[5] = 0;
    return 1;
}

/* From slice_win (glue.s), a DIGISLICER voice: 1 if it is stretched. */
int32_t ts_on(int32_t v)
{
    return v >= 0 && v < 8 && ts_rate(v) != 0;
}

/* From slice_win, after the window (a, b, as the render takes them: b < a reversed): kept for
 * ts_render, and handed back unchanged (d0:d1). */
uint64_t ts_win(int32_t v, uint32_t a, uint32_t b)
{
    ts_lo[v] = a;
    ts_hi[v] = b;
    ts_have[v] = 1;
    return ((uint64_t)a << 32) | b;
}

/* Track t's pitch rate, Q16.16: the stock voice's own, which the render works out every block before
 * the hook (the voice loop at 0x40075690 in 1.53) and keeps at V + 0x0C, Q27: TUNE after locks and
 * LFOs, the trig's note (SLICE's rule: with SLICE 0 the note picks the slice and plays at note 60),
 * through the pitch table, times the sample's rate ratio (V + 0x18). */
static uint32_t ts_pitch(int32_t t)
{
    return V_RATE(t) >> 11;
}

static void ts_config(int32_t t, uint32_t rs, uint32_t rp)
{
    if (rs == ts_rs[t] && rp == ts_rp[t] && ts_v[t].grain == dsl_ts_grain && ts_v[t].half == dsl_ts_half)
        return;
    ts_rs[t] = rs;
    ts_rp[t] = rp;
    dsl_config(&ts_v[t], rs, rp, dsl_ts_grain, dsl_ts_half);
}

/* The stock voice sounds TS_LATENCY samples after its start block (the DMA to the DSP and its
 * resampler), and ours is held back as long, so a stretched track keeps time with the others; and
 * the DSP's output is TS_GAIN (Q15) of the gain at V + 0x10. Both measured in the emulator
 * (STRETCH 1.0 against stock: correlation 0.999). */
#define TS_LATENCY  52
#define TS_GAIN     29295

static struct {
    int32_t wait;                       /* samples until the trig is played; -1 none */
    const int16_t *pcm;
    int32_t len, lo, hi, n;
    const int32_t *on;
} ts_pend[8];

/* ev_render_voices, order 50: interrupt level, every block. */
void ts_render(int32_t *blocks)
{
    uint32_t start = fw_voice_start;
    int32_t t, i, k;
    dsl_ts_blocks++;
    ts_budget = TS_SEARCHES;
    for (t = 0; t < 8; t++) {
        dsl_voice *v = &ts_v[t];
        int32_t *blk = blocks + 32 * t, g;
        uint32_t rs = core_track_machine[t] == DSL_ID ? ts_rate(t) : 0;
        dsl_ts_rate[t] = rs;
        if (!rs || core_track_machine[t] != DSL_ID) {
            ts_own[t] = 0;
            v->active = 0;
            ts_have[t] = 0;
            ts_pend[t].wait = -1;
            continue;
        }
        if (start >> t & 1) {
            int32_t busy = 0, u;
            for (u = 0; u < 8; u++)     /* the other tracks stretching now */
                busy += u != t && ts_own[u] && (ts_v[u].active || ts_pend[u].wait >= 0);
            if (ts_have[t] && ts_lo[t] < ts_hi[t] && busy < TS_MAX_VOICES) {
                uint32_t slot = V_SLOT(t), n = slot < 128 ? slc_n[slot] : 0;
                ts_pend[t].pcm = V_PCM(t);
                ts_pend[t].len = V_LEN(t);
                ts_pend[t].lo = (int32_t)ts_lo[t];
                ts_pend[t].hi = (int32_t)ts_hi[t];
                ts_pend[t].on = n ? (const int32_t *)slc_pts[slot] : 0;
                ts_pend[t].n = (int32_t)n;
                ts_pend[t].wait = TS_LATENCY;
                ts_own[t] = 1;
            } else {                    /* reversed, no window, or 4 stretching: stock */
                ts_own[t] = 0;
                v->active = 0;
                ts_pend[t].wait = -1;
            }
        }
        ts_have[t] = 0;
        if (!ts_own[t])
            continue;
        ts_config(t, rs, ts_pitch(t)); /* TUNE's locks and LFOs, STRETCH's changes */
        k = 0;
        if (ts_pend[t].wait >= 32) {    /* the old note plays on until the trig */
            ts_pend[t].wait -= 32;
            k = 32;
        } else if (ts_pend[t].wait >= 0) {
            k = ts_pend[t].wait;
        }
        if (k)
            dsl_render(v, ts_out, k);
        if (k < 32) {
            if (ts_pend[t].wait >= 0 && ts_pend[t].wait < 32) {
                v->budget = &ts_budget;
                v->stagger = (t * dsl_ts_grain) >> 3;
                dsl_trigger(v, ts_pend[t].pcm, ts_pend[t].len, ts_pend[t].lo, ts_pend[t].hi,
                            ts_pend[t].on, ts_pend[t].n);
                ts_pend[t].wait = -1;
            }
            dsl_render(v, ts_out + k, 32 - k);
        }
        g = ((V_GAIN(t) >> 16) * TS_GAIN) >> 15;   /* Q15 */
        for (i = 0; i < 32; i++)
            blk[i] = (ts_out[i] * g) << 1;  /* Q15 x Q15 -> Q31 */
    }
}
