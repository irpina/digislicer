/* SPDX-License-Identifier: GPL-2.0-or-later */
/* dsl_stretch: see dsl_stretch.h. Integers only; one divide per splice and per dsl_config. */
#pragma GCC optimize ("O2")             /* the render's hot loop: the mod builds -Os otherwise */
#include "dsl_stretch.h"

#define FADE_MIN    64                  /* splice fades: 1.3 ms ... */
#define FADE_MAX    1536                /* ... 32 ms */
#define PUNCH_FADE  48                  /* 1 ms: trigs and onsets */
#ifndef PEAK_STEP
#define PEAK_STEP   4                   /* the peak search's coarse step */
#endif

static int16_t ss[257];                 /* smoothstep x^2 (3 - 2x), Q15, x = i / 256 */
static int32_t ss_ok;

static void ss_init(void)
{
    int32_t i, s;
    for (i = 0; i <= 256; i++) {        /* i^2 (768 - 2i): Q16 x Q8 = Q24, >> 9 = Q15 */
        s = (i * i * (768 - 2 * i)) >> 9;
        ss[i] = (int16_t)(s > 32767 ? 32767 : s);
    }
    ss_ok = 1;
}

static inline int32_t absv(int32_t x) { return x < 0 ? -x : x; }

/* The smallest s with 1 << s >= step. */
static int32_t shift_for(uint32_t step)
{
    int32_t s = 0;
    while ((1u << s) < step)
        s++;
    return s;
}

/* p += r (Q16.16) */
static inline void adv(dsl_pos *p, uint32_t r)
{
    uint32_t f = p->f + (r << 16);
    p->i += (int32_t)(r >> 16) + (f < p->f);
    p->f = f;
}

/* A head's PCM, linear between samples; 0 outside its sample. */
static inline int32_t rd(const dsl_head *h)
{
    int32_t i = h->p.i, s0, s1;
    if ((uint32_t)i >= (uint32_t)(h->len - 1))
        return 0;
    s0 = h->pcm[i];
    s1 = h->pcm[i + 1];
    return s0 + (((s1 - s0) * (int32_t)(h->p.f >> 17)) >> 15);
}

/* The grid's next boundary: the next onset in the slice, or its end. */
static inline int32_t boundary(const dsl_voice *v)
{
    return v->next < v->non ? v->on[v->next] : v->b;
}

void dsl_config(dsl_voice *v, uint32_t rs, uint32_t rp, int32_t grain, int32_t half)
{
    int32_t d = absv((int32_t)rp - (int32_t)rs), t;
    if (!ss_ok)
        ss_init();
    v->rs = rs;
    v->rp = rp;
    v->grain = grain;
    v->half = half;
    /* output samples between splices when nothing else intervenes: grain / |rp - rs| */
    t = d ? (int32_t)(((uint32_t)grain << 16) / (uint32_t)d) : FADE_MAX;
    t = t * 3 / 4;
    v->fade = t < FADE_MIN ? FADE_MIN : t > FADE_MAX ? FADE_MAX : t;
    v->reach = (int32_t)(((uint32_t)v->fade * (rp >> 4)) >> 12) + 4;
    v->punch_fade = PUNCH_FADE;
    /* most a position's integer part moves in a sample: the grid, a head, their distance */
    v->gsh = shift_for((rs >> 16) + 1);
    v->hsh = shift_for((rp >> 16) + 1);
    v->dsh = shift_for(((uint32_t)d >> 16) + 2);
    if (!v->thresh || v->thresh > grain)
        v->thresh = grain;
}

/* After a trig or punch: the first splice comes v->stagger samples of drift early. */
static void rearm(dsl_voice *v)
{
    int32_t t = v->grain - v->stagger;
    v->thresh = t < 64 ? 64 : t;
}

/* A new live head (returned; the caller seats it) fading in over len samples. Whatever played
 * becomes the outgoing mix, as it sounded: the live head at its current weight, the heads that
 * were already fading out scaled by the rest. With all three heads busy, the quietest outgoing
 * one is dropped. */
static dsl_head *fade_in(dsl_voice *v, int32_t len)
{
    int32_t w = v->fading ? ss[v->fph >> 8] : 32768, j, n = -1;
    for (j = 0; j < DSL_HEADS; j++)
        if (j != v->live)
            v->head[j].w = v->fading ? (v->head[j].w * (32768 - w)) >> 15 : 0;
    v->head[v->live].w = w;
    for (j = 0; j < DSL_HEADS; j++)
        if (j != v->live && (n < 0 || v->head[j].w < v->head[n].w))
            n = j;
    if (v->head[n].w)
        v->n_three++;
    v->head[n].w = 0;
    v->head[n].pcm = v->pcm;
    v->head[n].len = v->len;
    v->live = n;
    v->fading = 1;
    v->fph = 0;
    v->fstep = 65536u / (uint32_t)len;
    v->fleft = (65536u + v->fstep - 1) / v->fstep;
    return &v->head[n];
}

void dsl_trigger(dsl_voice *v, const int16_t *pcm, int32_t len, int32_t a, int32_t b,
                 const int32_t *on, int32_t non)
{
    v->pcm = pcm;
    v->len = len;
    v->a = a;
    v->b = b;
    v->on = on;
    while (non > 0 && on[non - 1] >= b)     /* only the onsets inside the slice */
        non--;
    v->non = non;
    v->next = 0;
    while (v->next < non && on[v->next] <= a)
        v->next++;
    v->floor = a;
    v->grid.i = a;
    v->grid.f = 0;
    if (v->active) {                    /* the old heads fade out where they play */
        dsl_head *h = fade_in(v, v->punch_fade);
        int32_t j;
        if (v->tail)                    /* an ending slice goes out as loud as it was */
            for (j = 0; j < DSL_HEADS; j++)
                if (j != v->live)
                    v->head[j].w = v->head[j].w * v->tail / v->punch_fade;
        h->p = v->grid;
    } else {
        int32_t j;
        for (j = 0; j < DSL_HEADS; j++)
            v->head[j].w = 0;
        v->head[v->live].p = v->grid;
        v->head[v->live].pcm = pcm;
        v->head[v->live].len = len;
        v->fading = 0;
    }
    v->tail = 0;
    v->active = 1;
    rearm(v);
}

/* The highest point of pcm[c - h .. c + h] within [lo, hi], each lowered by its distance from c
 * (by `bias` at the window's edges). Returns its index, or c (kept inside the sample) if the
 * range is empty. */
static int32_t peak(const dsl_voice *v, int32_t c, int32_t lo, int32_t hi, int32_t bias)
{
    int32_t h = v->half, x, best, bs = -0x7fffffff, k;
    best = c < 0 ? 0 : c > v->len - 1 ? v->len - 1 : c;
    if (lo < c - h)
        lo = c - h;
    if (hi > c + h)
        hi = c + h;
    if (lo < 0)
        lo = 0;
    if (hi > v->len - 1)
        hi = v->len - 1;
    k = (bias << 8) / (h ? h : 1);      /* Q8 per sample */
    /* every PEAK_STEP-th sample, then the samples around the best of those */
    for (x = lo; x <= hi; x += PEAK_STEP) {
        int32_t s = v->pcm[x] - ((absv(x - c) * k) >> 8);
        if (s > bs) {
            bs = s;
            best = x;
        }
    }
    if (PEAK_STEP > 1 && bs != -0x7fffffff) {
        int32_t a = best - (PEAK_STEP - 1), b = best + (PEAK_STEP - 1);
        if (a < lo)
            a = lo;
        if (b > hi)
            b = hi;
        for (x = a; x <= b; x++) {
            int32_t s = v->pcm[x] - ((absv(x - c) * k) >> 8);
            if (s > bs) {
                bs = s;
                best = x;
            }
        }
    }
    return best;
}

/* Hand over to a new head, near the grid. guard: the live head is closing on the boundary
 * ahead of the grid, so the new one must start at or behind the grid. */
static void splice(dsl_voice *v, int32_t guard)
{
    const dsl_head *old = &v->head[v->live];
    int32_t h0 = old->p.i, g = v->grid.i, bnd = boundary(v), n;
    int32_t lo = v->floor, hi = bnd - v->reach;    /* where the new head may start */
    uint32_t f = old->p.f;
    dsl_head *h;
    if (guard && hi > g)
        hi = g;
    if (hi < lo)                        /* a segment shorter than a fade: start it over */
        hi = lo;
    if (v->half) {
        /* the old head's dominant peak half way through the fade, r samples ahead of it ... */
        int32_t c = h0 + (v->reach >> 1), pr, r, pt;
        pr = peak(v, c, v->floor, bnd - 1, 0);
        r = pr - h0;
        /* ... and the matching peak near the grid, so both heads cross their peaks together */
        pt = peak(v, g + r, lo + r, hi + r, absv(v->pcm[pr]) >> 1);
        n = pt - r;
    } else {
        n = g;
    }
    if (n < lo)
        n = lo;
    if (n > hi)
        n = hi;
    h = fade_in(v, v->fade);
    h->p.i = n;
    h->p.f = f;                         /* the old head's sub-sample phase */
#ifdef DSL_TRACE
    DSL_TRACE("splice guard %d head %d grid %d -> %d (lo %d hi %d bnd %d)", guard, h0, g, n, lo, hi, bnd);
#endif
    if (guard) {
        v->n_guard++;
    } else {
        v->n_splice++;
        v->thresh = v->grain;
    }
}

/* The grid reached an onset: a head onto it, now. */
static void punch(dsl_voice *v)
{
    v->floor = v->on[v->next];
    v->next++;
    fade_in(v, v->punch_fade)->p = v->grid;
    v->n_punch++;
    rearm(v);
}

/* The events (punch, slice end, splice, guard, fade end) are tested once per segment: at its
 * first sample, then the segment runs as long as bounds on the heads' and grid's integer steps
 * prove none can happen. Sample for sample the same as testing them every sample. */
void dsl_render(dsl_voice *v, int32_t *out, int32_t n)
{
    int32_t k = 0, j, m, t, bnd;
    while (k < n) {
        dsl_head *hl;
        if (!v->active) {
            out[k++] = 0;
            continue;
        }
        bnd = boundary(v);
        if (v->grid.i >= bnd && !v->tail) {
            if (v->next < v->non)
                punch(v);
            else
                v->tail = v->punch_fade;
            bnd = boundary(v);
        }
        hl = &v->head[v->live];
        if (!v->fading && !v->tail) {
            int32_t d = hl->p.i - v->grid.i;
            if (absv(d) > v->thresh && (!v->budget || *v->budget > 0)) {
                if (v->budget)
                    --*v->budget;
                splice(v, 0);
            } else if (d > 0 && hl->p.i + v->reach >= bnd)
                splice(v, 1);
            hl = &v->head[v->live];
        }

        /* the segment: no event can happen in it after its first sample */
        m = n - k;
        if (v->tail) {
            m = 1;
        } else {
            t = 1 + ((bnd - v->grid.i - 1) >> v->gsh);         /* the grid stays below bnd */
            if (t < m)
                m = t;
            if (v->fading) {
                if ((int32_t)v->fleft < m)
                    m = (int32_t)v->fleft;
            } else {
                t = v->thresh - absv(hl->p.i - v->grid.i);     /* no drift splice (or none */
                t = t < 0 ? m : 1 + (t >> v->dsh);              /* allowed until next block) */
                if (t < m)
                    m = t;
                t = bnd - v->reach - hl->p.i - 1;                /* no guard */
                t = t < 0 ? 1 : 1 + (t >> v->hsh);
                if (t < m)
                    m = t;
            }
        }

        if (!v->fading && !v->tail) {   /* one head: the common case */
            uint32_t rp = v->rp, rs = v->rs;
            for (j = 0; j < m; j++) {
                out[k + j] = rd(hl);
                adv(&hl->p, rp);
                adv(&v->grid, rs);
            }
        } else {
            for (j = 0; j < m; j++) {
                int32_t y = rd(hl), i;
                if (v->fading) {
                    int32_t w = ss[v->fph >> 8], o = 0;
                    for (i = 0; i < DSL_HEADS; i++) {
                        dsl_head *h = &v->head[i];
                        if (i == v->live || !h->w)
                            continue;
                        o += h->w == 32768 ? rd(h) : (rd(h) * h->w) >> 15;
                        adv(&h->p, v->rp);
                    }
                    y = (y * w + o * (32768 - w)) >> 15;
                    v->fph += v->fstep;
                    if (!--v->fleft) {
                        v->fading = 0;
                        for (i = 0; i < DSL_HEADS; i++)
                            v->head[i].w = 0;
                    }
                }
                if (v->tail) {
                    y = y * v->tail / v->punch_fade;
                    if (!--v->tail)
                        v->active = 0;
                }
                adv(&hl->p, v->rp);
                adv(&v->grid, v->rs);
                out[k + j] = y;
            }
        }
        k += m;
    }
}
