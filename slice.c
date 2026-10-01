/* SPDX-License-Identifier: GPL-2.0-or-later */
/* slice.c: automatic and custom slicing for the SLICE machine (GRID = AUTO).
 *
 * The stock SLICE machine (machine type 3) divides a sample into an equal
 * grid of 4..64 slices: its window function 0x40074df2 looks slice k up in
 * a table of 256 positions per sample slot (0x402F9380 + 0x400 * slot),
 * built at sample load. GRID = 5 ("AUTO", which stock firmware clamps to
 * 64) makes our hook in sysinfo.s (slice_win) call slice_auto_window
 * instead, which reads this file's table of slice starts for the slot.
 *
 * A slot's table comes from, in order:
 *   - the store of custom slice points (the slice editor's, kept per
 *     sample: its content hash and length), or
 *   - the analysis of the sample's transients.
 *
 * slice_tick runs from the 30 Hz compose hook (the UI task). It does
 * nothing until an AUTO voice first asks for a table (slc_any), so a song
 * with no AUTO track keeps the stock timing. Then it drops the table of
 * any slot whose sample changed, and fills the tables AUTO voices asked
 * for (slc_want): from the store at once, or by analysing a bounded
 * number of samples per tick. slc_n[slot] is written last and zeroed
 * first, so the render sees a whole table or none (and plays the 64 grid).
 */
#ifdef OS154                       /* the Digitakt mk1 1.54 (mod.json's port) */
#include "os154.h"
#else                              /* the Digitakt mk1 1.53 */
#include "os153.h"
#endif

typedef unsigned char u8;
typedef signed char s8;
typedef short s16;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;
typedef unsigned long long u64;

#define SLOTS     128
#define MAXSL     64
#define SMP_TAB   OS_SMP_TAB      /* +16 slot: PCM, rate, length, ratio */
#define REF_TAB   OS_REF_TAB      /* +16 slot: the loader's reference; +4 content hash */
#define VOICE_SLOT OS_VOICE_SLOT     /* V(v) + 0x5C, V = 0x8000EDC4 + 94 v */

#define HOP       128              /* samples per envelope hop (2.7 ms at 48 kHz) */
#define BUDGET    16384            /* samples analysed per tick */
#define GAP_HOPS  18               /* at least ~48 ms between slice starts */
#define MAXCAND   256
#define MINSL     64               /* the shortest slice the editor allows */

volatile u8 slc_n[SLOTS];          /* slices in the slot's table, 0 = none */
volatile u8 slc_want[SLOTS];       /* set by the render (slice_win): an AUTO voice
                                      started on this slot with no table */
volatile u8 slc_any;               /* set by any SLICE voice (slice_win): until then
                                      slice_tick does nothing, so a song with no
                                      SLICE track runs with the stock timing */
volatile u8 slc_cust[SLOTS];       /* the slot's table is the sample's own slice
                                      list: SLICE voices play it whatever GRID says */
u32 slc_pts[SLOTS][MAXSL];         /* slice starts, ascending, in samples */
static u32 slc_key[SLOTS][3];      /* PCM address, length, hash last seen */
static u8 slc_stale[SLOTS];        /* the sample changed since it was analysed */

/* ---- the store of custom slice points ------------------------------------ */
#define NREC      128
struct slc_rec {
    u32 hash, len;                 /* the sample: content hash, length in samples */
    u32 n;                         /* 0: free */
    u32 pts[MAXSL];
};
/* The store is kept on the +Drive (ekFS) as two files of this image,
 * /cfw/slices.a and /cfw/slices.b, written in turn (seq odd: a, even: b):
 * a save cut short by power loss leaves the other one whole, and the
 * loader takes the valid one with the higher seq. Each save rewrites a
 * file in place with one write (open "w" on an existing file keeps its
 * size and blocks): no directory change, no allocation. The 16 bytes of
 * padding keep the whole image in a USB listing, which reports size - 16. */
#define SLC_MAGIC 0x44545331u      /* "DTS1" */
struct slc_file {
    u32 magic, nrec, seq, sum;
    struct slc_rec rec[NREC];
    u8 pad[16];
};
struct slc_file slc_img;
#define slc_store (slc_img.rec)
volatile u32 slc_store_dirty;      /* changed since it was saved */

static struct slc_rec *store_find(u32 hash, u32 len)
{
    u32 i;
    for (i = 0; i < NREC; i++)
        if (slc_store[i].n && slc_store[i].hash == hash && slc_store[i].len == len)
            return &slc_store[i];
    return 0;
}

static struct slc_rec *store_put(u32 hash, u32 len, u32 n, const u32 *pts)
{
    struct slc_rec *r = store_find(hash, len);
    u32 i;
    if (!r)
        for (i = 0; i < NREC; i++)
            if (!slc_store[i].n) {
                r = &slc_store[i];
                break;
            }
    if (!r)
        return 0;                   /* full */
    r->hash = hash;
    r->len = len;
    for (i = 0; i < n; i++)
        r->pts[i] = pts[i];
    r->n = n;
    slc_store_dirty = 1;
    return r;
}

static void store_drop(u32 hash, u32 len)
{
    struct slc_rec *r = store_find(hash, len);
    if (r) {
        r->n = 0;
        slc_store_dirty = 1;
    }
}

/* ---- the store on the +Drive ------------------------------------------------ */
/* The firmware's own ekFS calls (they write straight to the card: no sync
 * needed). They take the filesystem's mutex themselves, except lookup; we
 * call them from the UI task, only while that mutex is free (a sample load
 * holds it for its whole read) and the drive is mounted. */
typedef struct { u32 inode, pos, writable, open; } ekfile;
#define EK_OPEN    ((s32 (*)(const char *, const char *, ekfile *))OS_EK_OPEN)
#define EK_READ    ((s32 (*)(void *, s32, ekfile *))OS_EK_READ)
#define EK_WRITE   ((s32 (*)(const void *, s32, ekfile *))OS_EK_WRITE)
#define EK_CLOSE   ((s32 (*)(ekfile *))OS_EK_CLOSE)
#define EK_MKDIR   ((s32 (*)(const char *))OS_EK_MKDIR)
#define EK_LOOKUP  ((s32 (*)(const char *, u32 *, s32, s32, s32))OS_EK_LOOKUP)
#define EK_LOCK    ((void (*)(void *))OS_EK_LOCK)
#define EK_UNLOCK  ((void (*)(void *))OS_EK_UNLOCK)
#define EK_MUTEX   ((void *)OS_EK_MUTEX)
#define EK_OWNER   (*(volatile u32 *)OS_EK_MUTEX)
#define EK_MOUNTED (*(volatile u32 *)OS_EK_MOUNTED)

static const char *const slc_path[2] = { "/cfw/slices.b", "/cfw/slices.a" };
static s32 slc_loaded;             /* 1: loaded (or found none) */
static s32 ed_is_open(void);
static s32 slc_save_wait;          /* ticks to wait before saving */
u32 slc_saves, slc_save_fail;      /* counts, for the USB PEEK */

static u32 img_sum(void)
{
    const u32 *w = (const u32 *)slc_img.rec;
    u32 n = sizeof slc_img.rec / 4, s = 0x5eed1234u, i;
    for (i = 0; i < n; i++)
        s = ((s << 5) | (s >> 27)) ^ w[i];
    return s;
}

static s32 ek_ready(void)
{
    return EK_MOUNTED != 0 && EK_OWNER == 0;
}

/* Read one file into slc_img; 1 if it is whole and valid. */
static s32 img_read(const char *path)
{
    ekfile f;
    s32 got;
    if (EK_OPEN(path, "r", &f) != 0)
        return 0;
    got = EK_READ(&slc_img, sizeof slc_img, &f);
    EK_CLOSE(&f);
    return got >= (s32)(sizeof slc_img - sizeof slc_img.pad) && slc_img.magic == SLC_MAGIC
           && slc_img.nrec == NREC && slc_img.sum == img_sum();
}

/* Load the store: the valid file with the higher seq, else an empty one.
 * Returns 0 if the drive is busy (try again later). */
static s32 store_load(void)
{
    s32 ok0, ok1, pick;
    u32 seq0, seq1, k;
    if (!ek_ready())
        return 0;
    ok0 = img_read(slc_path[0]);
    seq0 = slc_img.seq;
    ok1 = img_read(slc_path[1]);             /* slc_img now holds file 1 */
    seq1 = slc_img.seq;
    if (ok0 && ok1)
        pick = seq1 > seq0 ? 1 : 0;
    else
        pick = ok1 ? 1 : ok0 ? 0 : -1;
    if (pick == 0 && !img_read(slc_path[0]))
        pick = -1;
    if (pick < 0) {                          /* none: start empty */
        for (k = 0; k < NREC; k++)
            slc_img.rec[k].n = 0;
        slc_img.seq = 0;
    }
    slc_img.magic = SLC_MAGIC;
    slc_img.nrec = NREC;
    slc_loaded = 1;
    return 1;
}

/* Save the store over the older file (seq + 1 decides which). */
static s32 store_save(void)
{
    ekfile f;
    u32 ino, seq = slc_img.seq + 1;
    s32 r, w;
    if (!ek_ready())
        return 0;
    EK_LOCK(EK_MUTEX);
    r = EK_LOOKUP("/cfw", &ino, 0, 0, 0);
    EK_UNLOCK(EK_MUTEX);
    if (r == 0x19 && EK_MKDIR("/cfw") != 0)
        return -1;
    slc_img.magic = SLC_MAGIC;
    slc_img.nrec = NREC;
    slc_img.seq = seq;
    slc_img.sum = img_sum();
    if (EK_OPEN(slc_path[seq & 1], "w", &f) != 0)
        return -1;
    w = EK_WRITE(&slc_img, sizeof slc_img, &f);
    EK_CLOSE(&f);
    return w == (s32)sizeof slc_img ? 1 : -1;
}

/* From slice_tick: load on first use, save a second after the last edit. */
static void store_tick(void)
{
    if (!slc_loaded) {
        store_load();
        return;
    }
    if (!slc_store_dirty || ed_is_open())
        return;
    if (slc_save_wait > 0) {
        slc_save_wait--;
        return;
    }
    switch (store_save()) {
    case 1:
        slc_store_dirty = 0;
        slc_saves++;
        break;
    case -1:
        slc_save_fail++;
        slc_save_wait = 300;                 /* a failed save: try again in 10 s */
        break;
    default:                                 /* busy: next tick */
        break;
    }
}

/* ---- transient analysis ------------------------------------------------------ */
struct an {
    s32 active;
    s32 slot;
    const s16 *pcm;
    u32 len, key[3];
    s32 phase;                     /* 0 peak, 1 onsets, 2 done */
    u32 pos;
    s32 peak;
    s32 bg;                        /* background level x 256 */
    s32 prev;
    s32 hop_max;
    u32 hop_n;
    s32 last_onset;                /* hop index */
    u32 hop;
    u32 ncand;
    u32 cand_pos[MAXCAND];
    s32 cand_str[MAXCAND];
};
static struct an job;              /* the background analysis */
static u32 scan;                   /* the next slot slice_tick looks at */

static inline s32 absv(s32 x) { return x < 0 ? -x : x; }

static void slot_info(u32 slot, u32 *pcm, u32 *len, u32 *hash)
{
    const volatile u32 *e = (const volatile u32 *)(SMP_TAB + 16 * slot);
    const volatile u32 *r = (const volatile u32 *)(REF_TAB + 16 * slot);
    *pcm = e[0];
    *len = e[2];
    *hash = r[1];
}

static s32 slot_ok(u32 pcm, u32 len)
{
    return pcm >= 0x40000000u && pcm < 0x50000000u && len >= 2 * HOP && len <= 0x02000000u;
}

/* Where a transient at hop h starts: the first sample of hops h-1..h whose
 * level reaches a quarter of the hop's peak, moved back to the zero
 * crossing before it (within 256 samples). */
static u32 refine(const s16 *x, u32 len, u32 h, s32 hmax)
{
    u32 a = h > 0 ? (h - 1) * HOP : 0, b = (h + 1) * HOP, i, j;
    s32 thr = hmax >> 2;
    if (b > len)
        b = len;
    for (i = a; i < b; i++)
        if (absv(x[i]) >= thr)
            break;
    if (i >= b)
        i = h * HOP;
    for (j = i; j > 0 && i - j < 256; j--)
        if ((x[j - 1] <= 0 && x[j] > 0) || (x[j - 1] >= 0 && x[j] < 0) || x[j] == 0)
            break;
    return j;
}

static void an_start(struct an *a, u32 slot, u32 pcm, u32 len, u32 hash)
{
    a->slot = slot;
    a->pcm = (const s16 *)pcm;
    a->len = len;
    a->key[0] = pcm;
    a->key[1] = len;
    a->key[2] = hash;
    a->phase = 0;
    a->pos = 0;
    a->peak = 0;
    a->bg = 0;
    a->prev = 0;
    a->hop_max = 0;
    a->hop_n = 0;
    a->last_onset = -GAP_HOPS;
    a->hop = 0;
    a->ncand = 0;
    a->active = 1;
}

/* Up to `budget` samples of work; returns 1 when the analysis is done. */
static s32 an_step(struct an *a, u32 budget)
{
    const s16 *x = a->pcm;
    u32 end = a->pos + budget, i;
    if (end > a->len)
        end = a->len;
    if (a->phase == 0) {
        s32 peak = a->peak;
        for (i = a->pos; i < end; i++) {
            s32 v = absv(x[i]);
            if (v > peak)
                peak = v;
        }
        a->peak = peak;
        a->pos = end;
        if (end >= a->len) {
            a->phase = 1;
            a->pos = 0;
        }
        return 0;
    }
    if (a->phase == 1) {
        s32 floor = a->peak / 20 + 1;
        for (i = a->pos; i < end; i++) {
            s32 v = absv(x[i]);
            if (v > a->hop_max)
                a->hop_max = v;
            if (++a->hop_n < HOP)
                continue;
            {
                s32 e = a->hop_max, bg = a->bg >> 8;
                if (e > floor && e > 2 * bg + 1 && e > a->prev
                        && (s32)a->hop - a->last_onset >= GAP_HOPS && a->hop > 0) {
                    u32 p = refine(x, a->len, a->hop, e);
                    s32 str = e - bg;
                    if (a->ncand < MAXCAND) {
                        a->cand_pos[a->ncand] = p;
                        a->cand_str[a->ncand++] = str;
                    } else {                /* full: replace the weakest */
                        u32 w = 0, c;
                        for (c = 1; c < MAXCAND; c++)
                            if (a->cand_str[c] < a->cand_str[w])
                                w = c;
                        if (str > a->cand_str[w]) {
                            a->cand_pos[w] = p;
                            a->cand_str[w] = str;
                        }
                    }
                    a->last_onset = a->hop;
                }
                a->bg += ((e << 8) - a->bg) >> 3;
                a->prev = e;
                a->hop_max = 0;
                a->hop_n = 0;
                a->hop++;
            }
        }
        a->pos = end;
        if (end >= a->len)
            a->phase = 2;
        return 0;
    }
    return 1;
}

/* The finished analysis as a table: 0, then the strongest MAXSL-1
 * candidates, in order, at least a hop apart. Returns the count. */
static u32 an_table(struct an *a, u32 *pts)
{
    u32 n = 0, i, k;
    u8 used[MAXCAND];
    for (i = 0; i < a->ncand; i++)
        used[i] = 0;
    pts[n++] = 0;
    while (n < MAXSL) {
        s32 best = -1, bs = -1;
        for (i = 0; i < a->ncand; i++)
            if (!used[i] && a->cand_str[i] > bs) {
                bs = a->cand_str[i];
                best = i;
            }
        if (best < 0)
            break;
        used[best] = 1;
        pts[n++] = a->cand_pos[best];
    }
    for (i = 1; i < n; i++)                 /* insertion sort */
        for (k = i; k > 0 && pts[k - 1] > pts[k]; k--) {
            u32 t = pts[k];
            pts[k] = pts[k - 1];
            pts[k - 1] = t;
        }
    for (i = k = 1; i < n; i++)             /* drop duplicates and a start at 0 */
        if (pts[i] > pts[k - 1] + HOP)
            pts[k++] = pts[i];
    return k;
}

/* Publish a table for a slot, if its sample is still the one given. */
static void publish(u32 slot, const u32 *key, u32 n, const u32 *pts)
{
    u32 pcm, len, hash, i;
    slot_info(slot, &pcm, &len, &hash);
    if (pcm != key[0] || len != key[1] || hash != key[2])
        return;
    slc_n[slot] = 0;
    for (i = 0; i < n; i++)
        slc_pts[slot][i] = pts[i];
    slc_n[slot] = (u8)n;
}

void slc_ui_tick(void);
s32 slc_ui_playing(void);

/* ev_tick, 30 times a second (ctrl: the view controller). */
void slice_tick(u8 *ctrl)
{
    u32 n, pcm, len, hash;
#ifdef SLICE_NOTICK
    return;                                  /* test builds: no analysis */
#endif
    if (!slc_any)
        return;
    if (ed_is_open()) {
        slc_ui_tick();                       /* the editor's knobs (their dead zone) */
        if (slc_ui_playing())
            ctrl[0x20] = 1;                  /* the playhead moves: recompose */
    }
    store_tick();                            /* the custom points on the +Drive */
    if (!slc_loaded)
        return;                              /* none filled before they are known */
    /* Any slot whose sample changed loses its table at once (cheap: 128 x 3
     * reads), so the render never uses a stale one. */
    for (n = 0; n < SLOTS; n++) {
        slot_info(n, &pcm, &len, &hash);
        if (pcm == slc_key[n][0] && len == slc_key[n][1] && hash == slc_key[n][2])
            continue;
        slc_n[n] = 0;
        slc_cust[n] = 0;
        slc_key[n][0] = pcm;
        slc_key[n][1] = len;
        slc_key[n][2] = hash;
        slc_stale[n] = 1;
    }
    if (job.active) {
        if (an_step(&job, BUDGET)) {
            u32 pts[MAXSL], cnt = an_table(&job, pts);
            publish(job.slot, job.key, cnt, pts);
            job.active = 0;
        }
        return;
    }
    /* A sample with its own slice list gets it in every slot at once (any
     * SLICE track plays it); what AUTO tracks asked for is analysed. */
    for (n = 0; n < SLOTS; n++) {
        u32 s = scan;
        struct slc_rec *r;
        scan = (scan + 1) & (SLOTS - 1);
        if (!slc_stale[s])
            continue;
        if (!slot_ok(slc_key[s][0], slc_key[s][1])) {
            slc_stale[s] = 0;
            slc_cust[s] = 0;
            continue;                        /* empty or not in the pool: nothing to slice */
        }
        r = store_find(slc_key[s][2], slc_key[s][1]);
        if (r) {
            slc_stale[s] = 0;
            publish(s, slc_key[s], r->n, r->pts);
            slc_cust[s] = 1;
            continue;
        }
        slc_cust[s] = 0;
        if (!slc_want[s])
            continue;                        /* stays stale: analysed if AUTO asks */
        slc_stale[s] = 0;
        an_start(&job, s, slc_key[s][0], slc_key[s][1], slc_key[s][2]);
        return;
    }
}

/* The SLICE window for GRID = AUTO, called by slice_win (sysinfo.s) with
 * 0x40074df2's arguments once it has checked the slot has a table: p =
 * the voice's parameters (+2 PLAY, +8 SLICE, +10 LEN, high bytes), note =
 * the trig's note << 16, len = the sample's length, v = the voice. Returns
 * d0 = where the voice starts, d1 = where it ends, as the original: slice
 * k = SLICE - 1 (wrapped), or the note (- 12, wrapped) when SLICE is 0;
 * LEN more slices after it; reversed for PLAY 0 and 1. */
u64 slice_auto_window(const s8 *p, u32 note, u32 len, s32 v)
{
    u32 slot = *(const volatile u8 *)(VOICE_SLOT + 94 * v);
    s32 n = slc_n[slot], sel = (u8)p[8], k, L = p[10], idx;
    u32 from, to;
    extern volatile s32 slc_aud_voice, slc_aud_slice;
    extern void slc_dbg_win(s32 v, s32 sel, s32 aud);
    s32 aud = slc_aud_voice == v;
    if (aud) {                             /* the editor's audition: while a trig */
        sel = slc_aud_slice % n + 1;       /* key is held, every start of that */
        L = 0;                             /* track's voice plays its slice */
    }
    slc_dbg_win(v, sel, aud);
    /* SLICE 1..: slice (SLICE - 1) wrapped to the slices found, where the
     * grid clamps: how many slices AUTO finds depends on the sample, and
     * the lock helper's random locks run to 4 << GRID = 128. */
    if (sel > 0)
        k = (sel - 1) % n;
    else {
        k = ((s32)(s16)(note >> 16) - 12) % n;
        if (k < 0)
            k += n;
    }
    if (L > 63)
        L = 63;
    idx = k + L + 1;
    from = slc_pts[slot][k];
    to = idx >= n ? len : slc_pts[slot][idx < 0 ? 0 : idx];
    if ((u8)p[2] > 1)
        return ((u64)from << 32) | to;
    return ((u64)to << 32) | from;
}

/* ---- PIPO: ping-pong loops on the SLICE machine ------------------------------- */
/* PLAY (a voice's parameters + 2) is 0 REV, 1 REV.L, 2 FWD.L, 3 FWD; this mod
 * adds 4, PIPO: the parameter's range, its name and its icon are in glue.s.
 * The render calls the SLICE window function every block for every SLICE
 * voice (voice 0 from 0x40075184, voices 1-7 from the loop in 0x400757fe) and
 * reads PLAY straight after it, at one place in each (glue.s pp_read0 and
 * pp_readv), and once more for the loop flag alone (pp_read2). There a PIPO
 * voice reads as FWD.L or REV.L, as pp_dir says (FWD.L for the loop flag);
 * its PLAY itself stays 4. slice_win (pp_win, pp_step) turns the voice when it
 * nears an end of its window, and returns the window's ends in the order the
 * direction needs. Each note starts forward. Stock firmware plays PIPO as FWD.
 *
 * A turn made at a block's start would land about PP_K samples off where the
 * previous direction's output left off: the resampler's delay. So the
 * position moves on by PP_K at a turn, and the turn comes early enough (a
 * block's advance plus PP_K from the end) to keep it inside the window.
 * PP_K was measured in the emulator, whose resampler is the device's: with
 * it, every turn from -12 to +24 semitones steps as the waveform does. */
#define PP_PLAY   4
#define PP_K      14
#define PP_TRIG   (*(volatile u32 *)OS_PP_TRIG)              /* bit v: voice v starts */
#define PP_POS(v) (*(volatile s32 *)(OS_PP_POS + 94 * (v)))  /* V(v) + 4 */

s8 pp_dir[8];                              /* read by glue.s: 1 forward, -1 back */
static u8 pp_on[8], pp_have[8];
static s32 pp_last[8];

/* From slice_win, before the window: 1 if voice v plays PIPO. A note (or
 * PLAY just set to PIPO) starts it forward. */
s32 pp_pre(const u8 *p, s32 v)
{
    if (v < 0 || v > 7)
        return 0;
    if (p[2] != PP_PLAY) {
        pp_on[v] = 0;
        return 0;
    }
    if (!pp_on[v] || (PP_TRIG >> v & 1)) {
        pp_on[v] = 1;
        pp_dir[v] = 1;
        pp_have[v] = 0;
    }
    return 1;
}

/* From slice_win (pp_win), after the window (a, b: its ends, forward order):
 * turns the voice if it is due, and returns the window's ends in the order
 * its direction needs (d0:d1, the render's start and end). The position is
 * known only from the block after a note's first (the render sets it after
 * this call), so turns wait for two.
 *
 * A new window with no new note takes effect a block late. A trig's locks
 * reach the voice's parameters a block before its note starts, while the
 * old note fades out, so that block would give the old note the new note's
 * window: turned toward it, it played its last 32 samples backwards. Now
 * that block plays on in the old note's window. (A knob turned while a note
 * plays waits the same 0.67 ms.) */
static u32 pp_lo[8], pp_hi[8];             /* the window in use */
static u32 pp_nlo[8], pp_nhi[8];           /* the window asked for last block */

u64 pp_step(s32 v, u32 a, u32 b)
{
    s32 lo = a < b ? a : b, hi = a < b ? b : a, pos = PP_POS(v), adv;
    if (!pp_have[v] || ((u32)lo == pp_nlo[v] && (u32)hi == pp_nhi[v])) {
        pp_lo[v] = lo;
        pp_hi[v] = hi;
    }
    pp_nlo[v] = lo;
    pp_nhi[v] = hi;
    lo = pp_lo[v];
    hi = pp_hi[v];
    if (pp_have[v] > 1) {
        adv = pos - pp_last[v];
        if (adv < 0)
            adv = -adv;
        if (adv < 16 || adv > 4096)          /* none, or a jump: a block's worth */
            adv = 32;
        if (pp_dir[v] > 0 && pos + adv + PP_K >= hi) {
            pp_dir[v] = -1;
            PP_POS(v) = pos += PP_K;
        } else if (pp_dir[v] < 0 && pos - adv - PP_K <= lo) {
            pp_dir[v] = 1;
            PP_POS(v) = pos -= PP_K;
        }
    } else
        pp_have[v]++;
    pp_last[v] = pos;
    if (pp_dir[v] < 0)
        return ((u64)(u32)hi << 32) | (u32)lo;
    return ((u64)(u32)lo << 32) | (u32)hi;
}

/* PIPO's icon: the stock FWD and REV icons together (|<-->|), built once from
 * the PLAY icon set's own bitmaps. A Bitmap (BLIT 0x400c2960): +4 width, +8
 * height, +0xc words a column, +0x10 bits, +0x14 mask, 0x1c bytes. The OS
 * returns pointers in d0, where gcc looks for integers (it takes pointers
 * from a0), so the picker and pp_icon are declared to return a u32. */
#define ICON_SET  ((void *)OS_ICON_SET)
#define ICON_PICK ((u32 (*)(void *, s32, s32))OS_ICON_PICK)
static u32 pp_bm[7], pp_bits[64], pp_mask[64];
static s32 pp_bm_ok;

u32 pp_icon(void)
{
    const u32 *f, *r;
    u32 n, i;
    if (pp_bm_ok)
        return (u32)pp_bm;
    f = (const u32 *)ICON_PICK(ICON_SET, 3, 0);
    r = (const u32 *)ICON_PICK(ICON_SET, 0, 0);
    if (!f || !r)
        return (u32)f;
    n = f[1] * f[3];
    if (n > 64 || r[1] != f[1] || r[2] != f[2] || r[3] != f[3])
        return (u32)f;                       /* not the icons expected: FWD's */
    for (i = 0; i < 7; i++)
        pp_bm[i] = f[i];
    for (i = 0; i < n; i++) {
        pp_bits[i] = ((const u32 *)f[4])[i] | ((const u32 *)r[4])[i];
        pp_mask[i] = ((const u32 *)f[5])[i] | ((const u32 *)r[5])[i];
    }
    pp_bm[4] = (u32)pp_bits;
    pp_bm[5] = (u32)pp_mask;
    pp_bm_ok = 1;
    return (u32)pp_bm;
}

/* ---- the slice editor's model ------------------------------------------------- */
/* The editor works on a copy of one slot's table. Its view (drawn by the UI
 * side, sysinfo.s / the editor view) is a window [v0, v1) of the sample,
 * shown as WCOLS columns of min/max peaks. The screen's last 4 columns
 * hold the vertical zoom's slider. */
#define WCOLS 124
struct slc_ed {
    s32 open;
    u32 slot, pcm, len, hash;
    u32 n;
    u32 pts[MAXSL];
    s32 sel;                       /* the selected slice */
    u32 v0, v1;                    /* the view, in samples */
    s32 changed;                   /* differs from what was opened */
    s32 custom;                    /* the table came from (or goes to) the store */
    s32 cleared;                   /* DELETE ALL: the sample goes back to its grid */
    s16 lo[WCOLS], hi[WCOLS];      /* peaks per column, as samples */
    s32 peaks_ok;
    s32 zoom;                      /* the view spans len / 2^(zoom / ZSTEPS) */
    s32 vzoom;                     /* the waveform is drawn 2^(vzoom / ZSTEPS) high */
    u32 cur;                       /* the cursor, in samples: the selected slice's
                                      start, until LEVEL moves it */
};
struct slc_ed slc_ed;
static struct an ed_an;            /* the editor's (synchronous) analysis */

static s32 ed_is_open(void)
{
    return slc_ed.open;
}

static void ed_auto_table(void)
{
    an_start(&ed_an, slc_ed.slot, slc_ed.pcm, slc_ed.len, slc_ed.hash);
    while (!an_step(&ed_an, 1u << 20))
        ;
    ed_an.active = 0;
    slc_ed.n = an_table(&ed_an, slc_ed.pts);
}

static void ed_peaks(void)
{
    const s16 *x = (const s16 *)slc_ed.pcm;
    u32 span = slc_ed.v1 - slc_ed.v0, c;
    u32 q = span / WCOLS, r = span % WCOLS;     /* column c: v0 + c q + c r / WCOLS */
    for (c = 0; c < WCOLS; c++) {
        u32 a = slc_ed.v0 + c * q + (c * r) / WCOLS;
        u32 b = slc_ed.v0 + (c + 1) * q + ((c + 1) * r) / WCOLS, i, step;
        s32 lo = 0, hi = 0;
        if (b <= a)
            b = a + 1;
        step = (b - a) / 64 + 1;           /* at most ~64 reads a column */
        for (i = a; i < b && i < slc_ed.len; i += step) {
            s32 v = x[i];
            if (v < lo)
                lo = v;
            if (v > hi)
                hi = v;
        }
        slc_ed.lo[c] = (s16)lo;
        slc_ed.hi[c] = (s16)hi;
    }
    slc_ed.peaks_ok = 1;
}

/* Keep sample p in view: for a new selection a jump (p a quarter in), for
 * the cursor the least scroll that keeps it an eighth of the view from
 * either edge, so what comes next shows (the sample's own ends aside).
 * Scrolling right, p + m >= v1 >= span, so p + m + 1 - span does not
 * wrap. */
static void ed_show(u32 p, s32 jump)
{
    u32 span = slc_ed.v1 - slc_ed.v0, m = jump ? 0 : span / 8, v0;
    if (p >= slc_ed.v0 + m && p + m < slc_ed.v1)
        return;
    if (jump)
        v0 = p > span / 4 ? p - span / 4 : 0;
    else if (p < slc_ed.v0 + m)
        v0 = p > m ? p - m : 0;
    else
        v0 = p + m + 1 - span;
    if (v0 + span > slc_ed.len)
        v0 = slc_ed.len > span ? slc_ed.len - span : 0;
    slc_ed.v0 = v0;
    slc_ed.v1 = v0 + span;
    slc_ed.peaks_ok = 0;
}

/* The cursor goes to the selected slice's start, and the view follows. */
static void ed_follow(void)
{
    slc_ed.cur = slc_ed.pts[slc_ed.sel];
    ed_show(slc_ed.cur, 1);
}

/* Open the editor on a slot: its custom points, else its table (AUTO's
 * analysis or the stored points it plays), else a fresh analysis.
 * Returns 0 if the slot holds no sample. */
s32 slc_ed_open(u32 slot)
{
    u32 pcm, len, hash, i;
    struct slc_rec *r;
    if (slot >= SLOTS)
        return 0;
    slot_info(slot, &pcm, &len, &hash);
    if (!slot_ok(pcm, len))
        return 0;
    slc_ed.slot = slot;
    slc_ed.pcm = pcm;
    slc_ed.len = len;
    slc_ed.hash = hash;
    r = store_find(hash, len);
    if (r) {
        slc_ed.n = r->n;
        for (i = 0; i < r->n; i++)
            slc_ed.pts[i] = r->pts[i];
        slc_ed.custom = 1;
    } else if (slc_n[slot] && slc_key[slot][0] == pcm && slc_key[slot][1] == len
               && slc_key[slot][2] == hash) {
        slc_ed.n = slc_n[slot];
        for (i = 0; i < slc_ed.n; i++)
            slc_ed.pts[i] = slc_pts[slot][i];
        slc_ed.custom = 0;
    } else {
        ed_auto_table();
        slc_ed.custom = 0;
    }
    slc_ed.sel = 0;
    slc_ed.v0 = 0;
    slc_ed.v1 = len;
    slc_ed.zoom = 0;
    slc_ed.vzoom = 0;
    slc_ed.cur = slc_ed.pts[0];
    slc_ed.changed = 0;
    slc_ed.cleared = 0;
    slc_ed.peaks_ok = 0;
    slc_ed.open = 1;
    return 1;
}

/* Every slot holding the edited sample: apply f. */
static void ed_each_slot(void (*f)(u32 slot, const u32 *key))
{
    u32 s, pcm, len, hash, key[3];
    for (s = 0; s < SLOTS; s++) {
        slot_info(s, &pcm, &len, &hash);
        if (hash != slc_ed.hash || len != slc_ed.len || !slot_ok(pcm, len))
            continue;
        key[0] = pcm;
        key[1] = len;
        key[2] = hash;
        f(s, key);
    }
}

static void slot_own(u32 s, const u32 *key)      /* the edited list, as its own */
{
    slc_key[s][0] = key[0];
    slc_key[s][1] = key[1];
    slc_key[s][2] = key[2];
    slc_stale[s] = 0;
    publish(s, key, slc_ed.n, slc_ed.pts);
    slc_cust[s] = 1;
}

static void slot_back(u32 s, const u32 *key)     /* back to the grid (or AUTO) */
{
    (void)key;
    slc_cust[s] = 0;
    slc_n[s] = 0;
    slc_stale[s] = 1;
}

/* Close: keep the edits (store them for this sample, and play them now in
 * every slot holding it) or drop them. */
void slc_ed_close(s32 keep)
{
    if (!slc_ed.open)
        return;
    if (keep && slc_ed.cleared) {
        store_drop(slc_ed.hash, slc_ed.len);
        ed_each_slot(slot_back);
    } else if (keep && slc_ed.changed) {
        store_put(slc_ed.hash, slc_ed.len, slc_ed.n, slc_ed.pts);
        ed_each_slot(slot_own);
    }
    slc_ed.open = 0;
}

/* The edits so far, played at once: a changed list is the sample's own. */
static void ed_live(void)
{
    if (slc_ed.cleared) {
        ed_each_slot(slot_back);
        return;
    }
    if (slc_ed.changed)
        ed_each_slot(slot_own);
    else {
        u32 key[3];
        key[0] = slc_ed.pcm;
        key[1] = slc_ed.len;
        key[2] = slc_ed.hash;
        publish(slc_ed.slot, key, slc_ed.n, slc_ed.pts);
    }
}

/* CREATE GRID: an equal grid of g slices, each start moved back to the
 * zero crossing before it (within 128 samples), as the stock grid snaps. */
void slc_ed_grid(s32 g)
{
    const s16 *x = (const s16 *)slc_ed.pcm;
    u32 q, r, k;
    if (g < 1 || g > MAXSL)
        return;
    q = slc_ed.len / g;
    r = slc_ed.len % g;
    for (k = 0; k < (u32)g; k++) {
        u32 p = k * q + (k * r) / g, j;
        for (j = p; j > 0 && p - j < 128; j--)
            if ((x[j - 1] <= 0 && x[j] > 0) || (x[j - 1] >= 0 && x[j] < 0) || x[j] == 0)
                break;
        slc_ed.pts[k] = k ? j : 0;
    }
    slc_ed.n = g;
    slc_ed.sel = 0;
    slc_ed.changed = 1;
    slc_ed.cleared = 0;
    ed_follow();
}

/* DELETE ALL: one slice, the whole sample, and back to its grid on close. */
void slc_ed_clear(void)
{
    slc_ed.n = 1;
    slc_ed.pts[0] = 0;
    slc_ed.sel = 0;
    slc_ed.changed = 1;
    slc_ed.cleared = 1;
    ed_follow();
}

void slc_ed_select(s32 d)
{
    s32 s = slc_ed.sel + d;
    if (s < 0)
        s = 0;
    if (s >= (s32)slc_ed.n)
        s = slc_ed.n - 1;
    slc_ed.sel = s;
    ed_follow();
}

/* Move the selected slice's start by d steps: a column of the view
 * (coarse) or a sample (fine), kept between its neighbours. */
void slc_ed_move(s32 d, s32 fine)
{
    s32 k = slc_ed.sel;
    s32 step = fine ? 1 : (s32)((slc_ed.v1 - slc_ed.v0) / WCOLS);
    s32 p = (s32)slc_ed.pts[k] + d * (step ? step : 1);
    s32 lo = k > 0 ? (s32)slc_ed.pts[k - 1] + MINSL : 0;
    s32 hi = k + 1 < (s32)slc_ed.n ? (s32)slc_ed.pts[k + 1] - MINSL : (s32)slc_ed.len - MINSL;
    if (p < lo)
        p = lo;
    if (p > hi)
        p = hi;
    if ((u32)p != slc_ed.pts[k]) {
        slc_ed.pts[k] = p;
        slc_ed.cleared = 0;
        slc_ed.changed = 1;
    }
    ed_follow();
}

/* The zoom moves in steps of 1/ZSTEPS octave: the view's span at level z
 * is len / 2^(z / ZSTEPS), from the whole sample (0) down to no fewer
 * than two samples a column. zoom_frac[f] is 256 / 2^(f / ZSTEPS). */
#define ZSTEPS 16
static const u16 zoom_frac[ZSTEPS] = {
    256, 245, 235, 225, 215, 206, 197, 189, 181, 173, 166, 159, 152, 146, 140, 134
};

static u32 zoom_span(s32 z)
{
    u32 s = slc_ed.len >> (z / ZSTEPS), f = zoom_frac[z % ZSTEPS];
    return (s >> 8) * f + (((s & 255) * f) >> 8);
}

/* Zoom around the cursor by d steps (d > 0 in). */
void slc_ed_zoom(s32 d)
{
    s32 z = slc_ed.zoom + d;
    u32 span, c = slc_ed.cur;
    if (z < 0)
        z = 0;
    while (z > 0 && zoom_span(z) < WCOLS * 2)
        z--;
    if (z == slc_ed.zoom)
        return;
    slc_ed.zoom = z;
    span = zoom_span(z);
    slc_ed.v0 = c > span / 2 ? c - span / 2 : 0;
    if (slc_ed.v0 + span > slc_ed.len)
        slc_ed.v0 = slc_ed.len - span;
    slc_ed.v1 = slc_ed.v0 + span;
    slc_ed.peaks_ok = 0;
}

/* The vertical zoom, in the same steps (d > 0 taller): the waveform is
 * drawn up to 2^(VZMAX / ZSTEPS) = 64 times taller, and clipped. */
#define VZMAX (6 * ZSTEPS)
void slc_ed_vzoom(s32 d)
{
    s32 z = slc_ed.vzoom + d;
    slc_ed.vzoom = z < 0 ? 0 : z > VZMAX ? VZMAX : z;
}

/* Its gain x 256, 256 x 2^(vzoom / ZSTEPS): 2^(f / ZSTEPS) is
 * 2 zoom_frac[ZSTEPS - f] / 256. */
static s32 vzoom_gain(void)
{
    s32 k = slc_ed.vzoom / ZSTEPS, f = slc_ed.vzoom % ZSTEPS;
    return f ? zoom_frac[ZSTEPS - f] << (k + 1) : 256 << k;
}

/* The waveform's rows on the screen (slc_ui_draw): the peaks and slice
 * starts from WY0 to WY1, around WMID. */
#define WY0  18
#define WY1  50
#define WMID 34

/* A peak's height in pixels at gain g: full scale is WMID - WY0 = 16 at
 * gain 256. v g is at most 2^15 2^14, so it fits. */
static s32 peak_px(s32 v, s32 g)
{
    s32 y = (v * g) >> 19;
    return y > WMID - WY0 ? WMID - WY0 : y < WY0 - WMID ? WY0 - WMID : y;
}

/* Pan the view by half its width (d < 0 left). */
void slc_ed_pan(s32 d)
{
    u32 span = slc_ed.v1 - slc_ed.v0, half = span / 2;
    if (d < 0)
        slc_ed.v0 = slc_ed.v0 > half ? slc_ed.v0 - half : 0;
    else if (d > 0)
        slc_ed.v0 = slc_ed.v0 + half + span <= slc_ed.len ? slc_ed.v0 + half : slc_ed.len - span;
    slc_ed.v1 = slc_ed.v0 + span;
    slc_ed.peaks_ok = 0;
}

/* Split the selected slice at its middle. */
void slc_ed_add(void)
{
    s32 k = slc_ed.sel, i;
    u32 a = slc_ed.pts[k];
    u32 b = k + 1 < (s32)slc_ed.n ? slc_ed.pts[k + 1] : slc_ed.len;
    if (slc_ed.n >= MAXSL || b - a < 2 * MINSL)
        return;
    for (i = slc_ed.n; i > k + 1; i--)
        slc_ed.pts[i] = slc_ed.pts[i - 1];
    slc_ed.pts[k + 1] = a + (b - a) / 2;
    slc_ed.n++;
    slc_ed.sel = k + 1;
    slc_ed.cleared = 0;
    slc_ed.changed = 1;
    ed_follow();
}

/* Move the cursor by d columns of the view (LEVEL); the view scrolls to
 * keep it. d is at most a few thousand and a column 2^25 / WCOLS
 * samples, so d step fits. */
void slc_ed_cursor(s32 d)
{
    s32 step = (s32)((slc_ed.v1 - slc_ed.v0) / WCOLS);
    s32 c = (s32)slc_ed.cur + d * step;
    if (c < 0)
        c = 0;
    if (c > (s32)slc_ed.len - 1)
        c = slc_ed.len - 1;
    slc_ed.cur = c;
    ed_show(c, 0);
}

/* Where ADD SLICE HERE would put the new slice in the list: after the
 * starts at or before the cursor. -1 if a slice can't start there: at
 * MAXSL, or within MINSL of a start or of the end. */
static s32 ed_here(void)
{
    u32 c = slc_ed.cur, b;
    s32 k = 0;
    while (k < (s32)slc_ed.n && slc_ed.pts[k] <= c)
        k++;
    b = k < (s32)slc_ed.n ? slc_ed.pts[k] : slc_ed.len;
    if (slc_ed.n >= MAXSL || (k > 0 && c < slc_ed.pts[k - 1] + MINSL) || c + MINSL > b)
        return -1;
    return k;
}

/* ADD SLICE HERE: a slice starts at the cursor, and is selected. */
void slc_ed_add_here(void)
{
    s32 k = ed_here(), i;
    if (k < 0)
        return;
    for (i = slc_ed.n; i > k; i--)
        slc_ed.pts[i] = slc_ed.pts[i - 1];
    slc_ed.pts[k] = slc_ed.cur;
    slc_ed.n++;
    slc_ed.sel = k;
    slc_ed.cleared = 0;
    slc_ed.changed = 1;
    ed_follow();
}

/* Remove the selected slice: it merges into the one before (for the first
 * slice, what lay before the next one is no longer played). */
void slc_ed_delete(void)
{
    s32 k = slc_ed.sel, i;
    if (slc_ed.n <= 1)
        return;
    for (i = k; i + 1 < (s32)slc_ed.n; i++)
        slc_ed.pts[i] = slc_ed.pts[i + 1];
    slc_ed.n--;
    if (slc_ed.sel >= (s32)slc_ed.n)
        slc_ed.sel = slc_ed.n - 1;
    slc_ed.cleared = 0;
    slc_ed.changed = 1;
    ed_follow();
}

/* Back to the analysis (and forget this sample's custom points on keep). */
/* AUTO SLICE: the list becomes the analysis's (kept as the sample's own). */
void slc_ed_auto(void)
{
    ed_auto_table();
    slc_ed.sel = 0;
    slc_ed.changed = 1;
    slc_ed.cleared = 0;
    ed_follow();
}

/* The peaks for the current view (recomputed after a view change). */
const s16 *slc_ed_peaks(s32 which)
{
    if (!slc_ed.peaks_ok)
        ed_peaks();
    return which ? slc_ed.hi : slc_ed.lo;
}

/* Column (0..WCOLS-1) of a sample position in the view, or -1 outside it:
 * the column whose peaks (ed_peaks) cover it. Column c starts at
 * c span / WCOLS, so this is (x WCOLS + WCOLS - 1) / span, which fits 32
 * bits since span <= 2^25. */
s32 slc_ed_col(u32 pos)
{
    u32 span = slc_ed.v1 - slc_ed.v0, c;
    if (pos < slc_ed.v0 || pos >= slc_ed.v1)
        return -1;
    c = ((pos - slc_ed.v0) * WCOLS + WCOLS - 1) / span;
    return c < WCOLS ? (s32)c : WCOLS - 1;
}

/* ---- the slice editor's screen and controls ------------------------------------ */
/* Opened by pressing SRC on a DIGISLICER track's SRC page (slc_ui_srcpost,
 * from our wrapper of the SRC page's key handler): the page's waveform
 * view. While it is open the main loop's key and encoder events come here
 * first and most go no further (glue.s slc_key / slc_enc, slc_ui_key), and
 * slc_draw draws it over the whole screen after the views have drawn.
 *
 *   encoder A  select a slice        YES      the menu: ADD SLICE HERE (at
 *   encoder B  move its start                 the cursor), SPLIT SLICE, DELETE
 *   encoder C  ... by single samples          SLICE, AUTO SLICE, CREATE GRID
 *   encoder D  zoom (at the cursor)           (LEFT/RIGHT: 4..64), DELETE ALL
 *   encoder H  zoom vertically       FUNC+NO  delete the slice
 *   LEVEL      move the cursor (the view scrolls with it)
 *   trig keys  select and audition   NO, SRC  done (kept for this sample)
 *              slice 1-16 (UP/DOWN: 17-32, ...)
 *   LEFT/RIGHT the previous/next slice, played while held
 *   FUNC+LEFT/RIGHT the previous/next sample (the track's SAMP)
 *                                    PLAY, STOP  work as ever
 *
 * Edits are played at once (the slot's table is republished), and on NO
 * they are kept per sample in the store (slc_store) for every project. */
typedef void (*fillrect_t)(void *bmp, s32 x0, s32 y0, s32 x1, s32 y1, s32 c);
typedef void (*framerect_t)(void *bmp, s32 x0, s32 y0, s32 x1, s32 y1, s32 c);
typedef void (*vline_t)(void *bmp, s32 x, s32 y0, s32 y1, s32 c);
typedef void (*pixel_t)(void *bmp, s32 x, s32 y, s32 c);
typedef void (*textf_t)(void *bmp, const void *font, s32 x, s32 y, s32 maxlen, const char *fmt, ...);
typedef void (*noteon_t)(s32 track, s32 note, s32 vel, s32 src, s32 a, s32 b, s32 c);
typedef void (*noteoff_t)(s32 track, s32 note, s32 src);
typedef s32 (*machine_t)(void *view);
#define FILLRECT ((fillrect_t)OS_FILLRECT)   /* colour 0 clear, 1 set, < 0 invert */
#define FRAMERECT ((framerect_t)OS_FRAMERECT)
#define VLINE    ((vline_t)OS_VLINE)
#define PIXEL    ((pixel_t)OS_PIXEL)
#define TEXTF    ((textf_t)OS_TEXTF)
#define FONT5    ((const void *)OS_FONT5)
#define NOTEON   ((noteon_t)OS_NOTEON)
#define NOTEOFF  ((noteoff_t)OS_NOTEOFF)
#define MACHINE  ((machine_t)OS_MACHINE)    /* the SRC page's machine: 3 = SLICE */

#define K_YES   12
#define K_NO    13
#define K_UP    14
#define K_DOWN  15
#define K_LEFT  16
#define K_RIGHT 17
#define K_TRIG  19
#define K_SRC   20
#define K_FLTR  21
#define K_AMP   22
#define K_LFO   23
#define K_TRIG1 24
#define DSL_MACHINE 5                      /* DIGISLICER (glue.s dsl_machine) */

volatile s32 slc_aud_voice = -1;   /* while set, every start of this voice plays ... */
volatile s32 slc_aud_slice;        /* ... this slice (slice_auto_window) */

/* ---- diagnostics, read over USB (PEEK): the last 32 of each ------------------- */
struct slc_dbg {
    u32 enc_i, enc[32];            /* encoder << 24 | delta & 0xffffff */
    u32 key_i, key[32];            /* key << 16 | flags */
    u32 win_i, win[32];            /* audition << 31 | v << 24 | sel << 16 | slot */
    u32 noteons;
};
struct slc_dbg slc_dbg;

/* Called for every block a SLICE voice renders, not only at its start: an
 * entry the same as the last one is not logged again. */
void slc_dbg_win(s32 v, s32 sel, s32 aud)
{
    u32 slot = *(const volatile u8 *)(VOICE_SLOT + 94 * v);
    u32 x = (aud ? 0x80000000u : 0) | ((u32)v << 24) | ((u32)(sel & 0xff) << 16) | slot;
    if (slc_dbg.win_i && slc_dbg.win[(slc_dbg.win_i - 1) & 31] == x)
        return;
    slc_dbg.win[slc_dbg.win_i++ & 31] = x;
}
static s32 ui_page;                /* trig keys play slices 16 page + 1.. */
static s32 aud_track = -1, aud_note;

/* The SRC page the editor was opened from, and its track. */
typedef s32 (*trackof_t)(void *obj);
typedef void (*pageset_t)(void *view, s32 param, s32 delta, s32 flag, u8 *changed);
#define TRACK_OF ((trackof_t)OS_TRACK_OF)   /* (view + 116) -> the page's track */
#define P_SAMP   0x6f                      /* SAMP, as the page's setter knows it */
static void *ed_view;
static s32 ed_vtrack = -1;

/* The audio track playing the edited sample: the SRC page's track, else
 * the voice (one per track) that last started on its slot, else 0. */
static s32 ed_track(void)
{
    s32 t;
    if (ed_vtrack >= 0 && ed_vtrack < 8)
        return ed_vtrack;
    for (t = 0; t < 8; t++)
        if (*(const volatile u8 *)(VOICE_SLOT + 94 * t) == slc_ed.slot)
            return t;
    return 0;
}

static void aud_stop(void)
{
    slc_aud_voice = -1;
    if (aud_track >= 0)
        NOTEOFF(aud_track, aud_note, 0x40);
    aud_track = -1;
}

/* Play slice k on the edited sample's track. A note-off first makes the
 * note-on start the voice again even if something else just started it
 * (on the unit, a trig key can reach the track by another path too). */
static void aud_start(s32 k)
{
    aud_stop();
    aud_track = ed_track();
    aud_note = 60;
    slc_aud_slice = k;
    slc_aud_voice = aud_track;
    NOTEOFF(aud_track, aud_note, 0x40);
    NOTEON(aud_track, aud_note, 100, 0x40, 0, -1, -1);
    slc_dbg.noteons++;
}

static void knob_reset(void);
static void menu_reset(void);

/* After the SRC page's key handler: pressing SRC on the SRC page steps its
 * sub-page (view + 0x90: 0 the parameters, 1 the waveform view). On a
 * DIGISLICER track the waveform view is the editor, opened on the page's
 * sample slot (its grid widget's, which core sets up for DIGISLICER as
 * for SLICE). */
#define SUBPAGE(v) (*(s32 *)((u8 *)(v) + 0x90))
void slc_ui_srcpost(void *view)
{
    s32 slot;
    if (slc_ed.open) {
        if (view == ed_view && !SUBPAGE(view)) {
            aud_stop();                      /* SRC: back to the parameters */
            slc_ed_close(1);
        }
        return;
    }
    if (MACHINE(view) != DSL_MACHINE || !SUBPAGE(view))
        return;
    if (!slc_loaded && !store_load()) {
        SUBPAGE(view) = 0;                   /* the drive is busy: not now */
        return;
    }
    slot = *(const s32 *)((const u8 *)view + 524);  /* the SLICE waveform's slot */
    if (slot < 0 || slot >= SLOTS || !slc_ed_open(slot)) {
        SUBPAGE(view) = 0;
        return;
    }
    slc_any = 1;
    slc_want[slot] = 1;
    ed_live();
    ui_page = 0;
    knob_reset();
    menu_reset();
    ed_view = view;
    ed_vtrack = TRACK_OF(*(void **)((u8 *)view + 116));
}

/* Closing the editor (the edits kept, as ever) takes the SRC page back to
 * its parameters. */
static void ed_leave(void)
{
    aud_stop();
    slc_ed_close(1);
    if (ed_view)
        SUBPAGE(ed_view) = 0;
}

/* FUNC+LEFT/RIGHT: the previous or next sample slot that holds a sample. The
 * track's SAMP is moved there through the SRC page's own setter (its
 * vtable slot 22, 0x400309b0: what knob D calls, with a delta), the edits
 * so far are kept as NO keeps them, and the editor opens on the new
 * sample. */
static void ed_sample(s32 dir)
{
    s32 s = slc_ed.slot, from = s;
    u32 pcm, len, hash;
    u8 changed = 0;
    pageset_t set;
    if (!ed_view)
        return;
    for (;;) {
        s += dir;
        if (s < 1 || s >= SLOTS)
            return;                          /* none that way: stay */
        slot_info(s, &pcm, &len, &hash);
        if (slot_ok(pcm, len))
            break;
    }
    aud_stop();
    set = *(pageset_t *)(*(u8 **)ed_view + 88);
    set(ed_view, P_SAMP, (s - from) << 8, 0, &changed);
    slc_ed_close(1);
    if (!slc_ed_open(s))
        return;                              /* the editor stays shut */
    slc_want[s] = 1;
    ed_live();
    ui_page = 0;
    knob_reset();
    menu_reset();
}

/* The YES menu (as the Octatrack's slice menu). As there, ADD SLICE HERE
 * is listed only while a slice can start at the cursor, so with the
 * cursor on the selected slice's start the menu is SPLIT SLICE onwards. */
enum { M_ADD, M_SPLIT, M_DELETE, M_AUTO, M_GRID, M_ALL, M_ITEMS };
static const char *const menu_txt[M_ITEMS] = {
    "ADD SLICE HERE", "SPLIT SLICE", "DELETE SLICE", "AUTO SLICE", "CREATE GRID", "DELETE ALL"
};
static const u8 menu_grid[5] = { 4, 8, 16, 32, 64 };
static u8 menu_item[M_ITEMS];      /* the items listed, from menu_build */
static s32 menu_open, menu_sel, menu_n, menu_g = 2;

static void menu_reset(void)
{
    menu_open = 0;
    menu_sel = 0;
}

static void menu_build(void)
{
    s32 i;
    menu_n = 0;
    for (i = 0; i < M_ITEMS; i++)
        if (i != M_ADD || ed_here() >= 0)
            menu_item[menu_n++] = (u8)i;
    menu_sel = 0;
    menu_open = 1;
}

static void menu_do(void)
{
    switch (menu_item[menu_sel]) {
    case M_ADD: slc_ed_add_here(); break;
    case M_SPLIT: slc_ed_add(); break;
    case M_DELETE: slc_ed_delete(); break;
    case M_AUTO: slc_ed_auto(); break;
    case M_GRID: slc_ed_grid(menu_grid[menu_g]); break;
    case M_ALL: slc_ed_clear(); break;
    }
    ed_live();
    menu_open = 0;
}

static void menu_key(s32 key)
{
    switch (key) {
    case K_UP:
        if (menu_sel > 0)
            menu_sel--;
        break;
    case K_DOWN:
        if (menu_sel < menu_n - 1)
            menu_sel++;
        break;
    case K_LEFT:
        if (menu_item[menu_sel] == M_GRID && menu_g > 0)
            menu_g--;
        break;
    case K_RIGHT:
        if (menu_item[menu_sel] == M_GRID && menu_g < 4)
            menu_g++;
        break;
    case K_YES:
        menu_do();
        break;
    case K_NO:
        menu_open = 0;
        break;
    }
}

/* A key while the editor is open -> 1 if the editor takes it.
 * - SRC goes on to the SRC page, which steps back to its parameters
 *   (slc_ui_srcpost then closes the editor); FUNC+SRC closes it first and
 *   goes on to the machine menu.
 * - TRIG, FLTR, AMP and LFO close it and go on to their pages.
 * - Everything else is the editor's. */
s32 slc_ui_key(const u8 *ev)
{
    s32 key = *(const s32 *)(ev + 12), fl = *(const s32 *)(ev + 16);
    s32 press = (fl & 1) && !(fl & 8), func = fl & 2;
    slc_dbg.key[slc_dbg.key_i++ & 31] = ((u32)key << 16) | (fl & 0xffff);
    if (key == K_SRC && !func)
        return 0;
    if (key == K_SRC || key == K_TRIG || key == K_FLTR || key == K_AMP || key == K_LFO) {
        if (fl & 1)
            ed_leave();
        return 0;
    }
    if (menu_open) {
        if (press)
            menu_key(key);
        return 1;
    }
    if (key >= K_TRIG1 && key < K_TRIG1 + 16) {
        s32 k = ui_page * 16 + key - K_TRIG1;
        if (press && k < (s32)slc_ed.n) {
            slc_ed.sel = k;
            ed_follow();
            aud_start(k);
        } else if (!(fl & 1))
            aud_stop();
        return 1;
    }
    /* LEFT/RIGHT: the previous/next slice, played while held (as a trig
     * key; held, they repeat). FUNC+LEFT/RIGHT: the previous/next sample. */
    if (key == K_LEFT || key == K_RIGHT) {
        s32 d = key == K_LEFT ? -1 : 1;
        if (press && func)
            ed_sample(d);
        else if ((fl & 1) && !func) {
            slc_ed_select(d);
            aud_start(slc_ed.sel);
        } else if (!(fl & 1))
            aud_stop();
        return 1;
    }
    if (!press)
        return 1;
    switch (key) {
    case K_YES:
        if (func) {
            slc_ed_auto();
            ed_live();
        } else
            menu_build();
        break;
    case K_NO:
        if (func) {
            slc_ed_delete();
            ed_live();
        } else
            ed_leave();
        break;
    case K_UP:
        if (ui_page < 3)
            ui_page++;
        break;
    case K_DOWN:
        if (ui_page > 0)
            ui_page--;
        break;
    }
    return 1;
}

/* Select (A, and the menu) steps as the stock list parameters do (SAMP,
 * PLAY MODE): through the firmware's own knob filter 0x400c05ee
 * (state, event, config) with its default config 0x4208cb64. That config
 * notches: a knob's level rests at 48, a count takes 3 off it, nothing
 * steps above 24, then a step every 6 counts, and each step puts 48 back.
 * The state is ours: 20 bytes a knob from +32, level at +16 (knob 10 and
 * up, and PAGE, share +232). Stock builds it with 0x400c02e4 (levels 48)
 * and runs its tick 0x400c03cc on a period-10 timer, which counts a level
 * below 48 back up (the dead zone returns after a pause); slc_ui_tick runs
 * that tick 3 times a 30 Hz UI tick. The moves (B) sum counts, a step
 * every 4, keeping the acceleration; the fine move (C) is a sample a
 * count.
 *
 * The zoom (D) is not a list: through the notch filter it moved an octave
 * a notch, with a dead zone before each one, which was far too slow on
 * the unit. It takes every count instead, as 1/16 of an octave, and a
 * faster turn (more counts an event: the unit sent 1 to 21) counts for
 * more: an event of d counts zooms d + d |d| / 8 steps. So 1 count is
 * 1 step (4%), 4 counts 6, 8 counts 16 (an octave), 21 counts 76. The
 * vertical zoom (H, below D) takes the same steps, and so does the cursor
 * (LEVEL, as the Octatrack's waveform marker), a step a view column. */
static s32 zoom_steps(s32 d)
{
    s32 a = d < 0 ? -d : d;
    return d + d * a / 8;
}
typedef s32 (*knobf_t)(void *st, const u8 *ev, const void *cfg);
typedef void (*knobt_t)(void *st, void *timeout);
#define KNOB_FILTER ((knobf_t)OS_KNOB_FILTER)
#define KNOB_TICK   ((knobt_t)OS_KNOB_TICK)
#define KNOB_NOTCH  ((const void *)OS_KNOB_NOTCH)
static s32 knob_st[64];

static void knob_reset(void)
{
    s32 i;
    for (i = 0; i < 64; i++)
        knob_st[i] = 0;
    for (i = 0; i < 11; i++)
        knob_st[(32 + 20 * i + 16) / 4] = 48;
}

void slc_ui_tick(void)
{
    KNOB_TICK(knob_st, 0);
    KNOB_TICK(knob_st, 0);
    KNOB_TICK(knob_st, 0);
}

static s32 enc_acc[10];

static s32 enc_counts(s32 enc, s32 d, s32 cap)
{
    s32 st;
    if (enc < 0 || enc > 9)
        return 0;
    if ((d > 0 && enc_acc[enc] < 0) || (d < 0 && enc_acc[enc] > 0))
        enc_acc[enc] = 0;                  /* a change of direction starts afresh */
    enc_acc[enc] += d;
    st = enc_acc[enc] / 4;
    enc_acc[enc] -= st * 4;
    if (cap && st > cap)
        st = cap;
    if (cap && st < -cap)
        st = -cap;
    return st;
}

void slc_ui_enc(const u8 *ev)
{
    s32 enc = *(const s32 *)(ev + 12), d = *(const s32 *)(ev + 16);
    s32 steps;
    slc_dbg.enc[slc_dbg.enc_i++ & 31] = ((u32)enc << 24) | ((u32)d & 0xffffff);
    if (enc == 1)
        steps = KNOB_FILTER(knob_st, ev, KNOB_NOTCH);
    else if (enc == 4 || enc == 8 || enc == 9)
        steps = zoom_steps(d);
    else
        steps = enc == 3 ? d : enc_counts(enc, d, 0);
    if (steps && menu_open) {
        if (enc == 1) {
            menu_sel += steps;
            if (menu_sel < 0)
                menu_sel = 0;
            if (menu_sel > menu_n - 1)
                menu_sel = menu_n - 1;
        }
        return;
    }
    if (!steps)
        return;
    switch (enc) {
    case 1:
        slc_ed_select(steps);
        break;
    case 2:
        slc_ed_move(steps, 0);
        ed_live();
        break;
    case 3:
        slc_ed_move(steps, 1);
        ed_live();
        break;
    case 4:
        slc_ed_zoom(steps);
        break;
    case 8:
        slc_ed_vzoom(steps);
        break;
    case 9:
        slc_ed_cursor(steps);
        break;
    }
}

/* The screen, y = 0 at the bottom. The font is 4 pixels a character, so a
 * row holds 32.
 *   57..61  the selected slice and how many, the sample slot, the trig
 *           keys' page
 *   55      the horizontal zoom: the part of the sample in view
 *   54..56  the playhead's mark on it
 *   15..53  the waveform, columns 0..WCOLS-1: peaks WY0..WY1 around WMID,
 *           slice starts, the selected slice (its start WY0-2..WY1+2 and
 *           bars), the cursor (an I-beam), the playhead (inverted,
 *           WY0-3..WY1+3); the vertical zoom's slider right of it,
 *           x 125..127
 *   8..12   knobs A-D, in four columns as the stock pages lay out their
 *   1..5    parameters, and under them LEVEL (the cursor, and where it is)
 *           and H (under D); while the menu is open, the menu's keys */
static const char *const knob_txt[4] = { "A:SEL", "B:MOVE", "C:FINE", "D:ZOOM X" };

/* The playhead: where each voice playing the edited sample is, as the
 * render left it (V(v) + 4) when the frame is drawn. slice_tick recomposes
 * the frame at 30 Hz while there is one, and once more to erase it. */
#define VOICE_ON(v) (*(const volatile u8 *)(OS_VOICE_ON + 94 * (v)))   /* V(v) + 0x28 */
static s32 ph_shown;

static u32 ph_voices(void)                 /* a bit for each voice playing it */
{
    u32 m = 0;
    s32 v;
    for (v = 0; v < 8; v++)
        if (VOICE_ON(v) && *(const volatile u8 *)(VOICE_SLOT + 94 * v) == slc_ed.slot
            && (u32)PP_POS(v) < slc_ed.len)
            m |= 1u << v;
    return m;
}

s32 slc_ui_playing(void)
{
    return ph_voices() != 0 || ph_shown;
}

void slc_ui_draw(void *bmp)
{
    const s16 *lo = slc_ed_peaks(0), *hi = slc_ed_peaks(1);
    s32 c, i, a, b, g = vzoom_gain(), y, cx, px;
    u32 ph;
    u32 p = slc_ed.pts[slc_ed.sel];
    u32 end = slc_ed.sel + 1 < (s32)slc_ed.n ? slc_ed.pts[slc_ed.sel + 1] : slc_ed.len;
    FILLRECT(bmp, 0, 0, 127, 63, 0);
    TEXTF(bmp, FONT5, 1, 57, -1, "SL %d/%d %s", slc_ed.sel + 1, slc_ed.n,
          slc_ed.cleared ? "GRID" : slc_ed.changed ? "EDIT" : (slc_ed.custom ? "USER" : "AUTO"));
    TEXTF(bmp, FONT5, 65, 57, -1, "SMP%d", slc_ed.slot);
    if (ui_page)
        TEXTF(bmp, FONT5, 117, 57, -1, "P%d", ui_page + 1);
    /* The zooms, as the Octatrack shows them. Horizontal: the part of the
     * sample in view, a bar on a dotted line for the whole sample (v0 WCOLS
     * fits 32 bits as in slc_ed_col). Vertical: a slider right of the
     * waveform, a box that fills from the bottom as the waveform grows. */
    for (c = 0; c < WCOLS; c += 2)
        PIXEL(bmp, c, 55, 1);
    FILLRECT(bmp, slc_ed.v0 * WCOLS / slc_ed.len, 55, (slc_ed.v1 - 1) * WCOLS / slc_ed.len, 55, 1);
    FRAMERECT(bmp, 125, WY0, 127, WY1, 1);
    y = WY0 + (WY1 - WY0 - 1) * slc_ed.vzoom / VZMAX;
    if (y > WY0)
        VLINE(bmp, 126, WY0 + 1, y, 1);
    for (c = 0; c < WCOLS; c++) {
        s32 y0 = WMID + peak_px(lo[c], g), y1 = WMID + peak_px(hi[c], g);
        if (y1 < y0)
            y1 = y0;
        VLINE(bmp, c, y0, y1, 1);
    }
    cx = slc_ed_col(slc_ed.cur);           /* the cursor, off the start: the */
    if (cx >= 0 && slc_ed.cur != p)        /* waveform inverted */
        FILLRECT(bmp, cx, WY0, cx, WY1, -1);
    for (i = 0; i < (s32)slc_ed.n; i++) {  /* slice starts: dotted, cut out */
        s32 x = slc_ed_col(slc_ed.pts[i]);
        if (x < 0)
            continue;
        VLINE(bmp, x, WY0, WY1, 0);
        for (y = WY0; y <= WY1; y += 2)
            PIXEL(bmp, x, y, 1);
    }
    a = slc_ed_col(p);                     /* the selected slice: a solid start */
    b = end >= slc_ed.v1 ? WCOLS - 1 : slc_ed_col(end);   /* and bars over it */
    if (a >= 0)
        VLINE(bmp, a, WY0 - 2, WY1 + 2, 1);
    if (a < 0 && p < slc_ed.v0)
        a = 0;
    if (a >= 0 && b >= a) {
        FILLRECT(bmp, a, WY0 - 3, b, WY0 - 2, 1);
        FILLRECT(bmp, a, WY1 + 2, b, WY1 + 3, 1);
    }
    if (cx >= 0) {                         /* the cursor's ends: an I-beam */
        FILLRECT(bmp, cx > 0 ? cx - 1 : 0, WY0 - 1, cx + 1, WY0 - 1, 1);
        FILLRECT(bmp, cx > 0 ? cx - 1 : 0, WY1 + 1, cx + 1, WY1 + 1, 1);
    }
    ph = ph_voices();                      /* the playheads: a line through the */
    ph_shown = ph != 0;                    /* band, a mark on the view line */
    for (i = 0; i < 8; i++) {
        u32 pos;
        if (!(ph >> i & 1))
            continue;
        pos = (u32)PP_POS(i);
        px = slc_ed_col(pos);
        if (px >= 0)
            FILLRECT(bmp, px, WY0 - 3, px, WY1 + 3, -1);
        px = pos * WCOLS / slc_ed.len;
        FILLRECT(bmp, px, 54, px, 56, -1);
    }
    if (menu_open)
        TEXTF(bmp, FONT5, 1, 1, -1, "UP/DN YES:DO NO:BACK");
    else {
        for (i = 0; i < 4; i++)
            TEXTF(bmp, FONT5, 1 + 32 * i, 8, -1, "%s", knob_txt[i]);
        TEXTF(bmp, FONT5, 1, 1, -1, "LVL:CURSOR %d.%03ds", slc_ed.cur / 48000, (slc_ed.cur % 48000) / 48);
        TEXTF(bmp, FONT5, 97, 1, -1, "H:ZOOM Y");
    }
    if (menu_open) {                       /* a row every 7 pixels from 46 down, */
        s32 bot = 46 - 7 * (menu_n - 1) - 3;   /* the box fitted to them */
        FILLRECT(bmp, 16, bot, 111, 54, 0);
        FRAMERECT(bmp, 16, bot, 111, 54, 1);
        for (i = 0; i < menu_n; i++) {
            y = 46 - 7 * i;
            if (menu_item[i] == M_GRID)
                TEXTF(bmp, FONT5, 21, y, -1, "CREATE GRID <%d>", menu_grid[menu_g]);
            else
                TEXTF(bmp, FONT5, 21, y, -1, "%s", menu_txt[menu_item[i]]);
            if (i == menu_sel)
                FILLRECT(bmp, 18, y - 1, 109, y + 5, -1);
        }
    }
}
