/* SPDX-License-Identifier: GPL-2.0-or-later */
/* kbd.c: the keyboard's trig slice mode on DIGISLICER tracks.
 *
 * With the keyboard on (FUNC+TRK), the trig keys play the active track.
 * The firmware has a "fold slice" layout for it: on a SLICE track whose
 * SLICE is 0 (the keyboard icon) and FOLD on, trig key k of slice page p
 * plays note 12 + 16p + k, and SLICE 0 makes that note pick slice 16p + k.
 * On a DIGISLICER track with the keyboard on, that layout is the
 * keyboard's, whatever SLICE and FOLD say (glue.s has the sites), as the
 * Octatrack's trig slice mode:
 *   - trig key k plays slice 16p + k + 1, at the track's pitch: its note
 *     gets a lock of SLICE = 0, which picks the slice by the note and
 *     plays it at note 60 (with TUNE);
 *   - the keys of the sample's slices are lit, the others dark: its own
 *     slices or AUTO's when the track plays them, else 4 << GRID;
 *   - UP/DOWN steps the slice page, over as many pages as there are slices;
 *   - live recording (REC+PLAY) records each key as a trig with a SLICE
 *     lock of its slice and the track's NOTE, as a trig entered on the grid
 *     with that lock would be.
 * With the keyboard off, or on any other machine, nothing changes.
 */
typedef unsigned char u8;
typedef signed char s8;
typedef short s16;
typedef unsigned short u16;
typedef unsigned int u32;
typedef int s32;

#define SLOTS       128
#define DSL_MACHINE 5                      /* DIGISLICER (glue.s dsl_machine) */
#define SRC_KEYS    0x40                   /* NOTEON's source: the trig keys */
#define P_SLICE     21                     /* SLICE's lock slot */

extern volatile u8 slc_n[SLOTS], slc_cust[SLOTS], slc_want[SLOTS], slc_any;

typedef u32 (*fn0_t)(void);
typedef u32 (*fn1_t)(u32);
typedef u32 (*fn2_t)(u32, u32);
typedef s32 (*lockset_t)(u32 trk, s32 step, s32 slot, s32 value);
typedef s32 (*noteset_t)(u32 trk, s32 step, s32 note);
#define APP      ((fn0_t)0x4013865a)
#define TSTATE   ((fn1_t)0x40014d86)       /* (app) -> the TrackState */
#define KB_ON    ((fn1_t)0x4001d4f0)       /* (TrackState) -> the keyboard is on (a byte) */
#define ACTIVE   ((fn1_t)0x4001d27e)       /* (TrackState) -> the active track */
#define KIT      ((fn1_t)0x40014d92)       /* (app) -> the kit */
#define SOUND    ((fn2_t)0x4000d7be)       /* (kit, t) -> track t's sound */
#define PATTERN  ((fn1_t)0x40015786)       /* (app) -> the pattern */
#define PTRACK   ((fn2_t)0x40012a02)       /* (pattern, t) -> its track t */
#define LOCKSET  ((lockset_t)0x40024768)   /* a p-lock on a step: value = n << 8 */
#define NOTESET  ((noteset_t)0x40024942)   /* a trig's NOTE: -1 = the track's */
#define LOCKLIST ((fn0_t)0x400edfe6)       /* a lock list for a voice, emptied */
#define PREVIEW  (*(volatile u32 *)0x4020c29c)  /* the preview step, <= 63 when one */

/* A sound's or a pattern track's data: its object's vtable slot 10. */
static u8 *odata(u32 obj)
{
    u32 (*get)(u32) = *(u32 (**)(u32))(*(u32 *)obj + 40);
    return (u8 *)get(obj);
}

/* The sound's data: the machine at +0x7E, the parameters at +0x14 + 2 slot,
 * their integer in the high byte: SAMP (20) the sample slot, GRID (23). */
#define S_MACHINE 0x7e
#define S_SAMP    0x3c
#define S_GRID    0x42

/* Track t's sound if t is an audio track on DIGISLICER, else 0. */
static const u8 *dsl_sound(u32 app, s32 t)
{
    const u8 *snd;
    if (t < 0 || t > 7)
        return 0;
    snd = odata(SOUND(KIT(app), t));
    return snd && snd[S_MACHINE] == DSL_MACHINE ? snd : 0;
}

/* The slices the track plays, as slice_win picks them: the sample's own
 * slices or AUTO's table, else the grid. AUTO with no table yet asks for
 * one (slice_tick) and counts the 64 grid meanwhile, as it plays. */
static s32 slices(const u8 *snd)
{
    u32 slot = snd[S_SAMP], grid = snd[S_GRID];
    if (slot < SLOTS && slc_n[slot] && (slc_cust[slot] || grid == 5))
        return slc_n[slot];
    if (grid == 5 && slot < SLOTS) {
        slc_want[slot] = 1;
        slc_any = 1;
    }
    return 4 << (grid > 4 ? 4 : grid);
}

/* The slices of the track the keyboard plays, if it is on and that track
 * is on DIGISLICER: the trig slice mode is on. Else 0. */
static s32 kb_slices(s32 track)
{
    u32 app = APP(), ts = TSTATE(app);
    const u8 *snd;
    if (!(KB_ON(ts) & 0xff) || (s32)ACTIVE(ts) != track)
        return 0;
    snd = dsl_sound(app, track);
    return snd ? slices(snd) : 0;
}

/* The slices the keyboard's last fold slice test found (glue.s dsl_kbpred),
 * 0 when it was not on DIGISLICER: the key, LED and page hooks run after
 * that test, in the same call. */
volatile s32 dsl_kb_n;

/* The fold slice test (0x40028f3c and its copy): 1 on a DIGISLICER track
 * with the keyboard on; 0 runs the stock test. */
s32 dsl_kb_check(void)
{
    u32 ts = TSTATE(APP());
    dsl_kb_n = kb_slices((s32)ACTIVE(ts));
    return dsl_kb_n != 0;
}

/* UP/DOWN's new slice page (0x40025344 refuses past 3, and below 0): on
 * DIGISLICER a page past the last slice's goes to that page. */
s32 dsl_kb_page(s32 page)
{
    s32 last;
    if (!dsl_kb_n || page < 0 || page > 3)
        return page;
    last = (dsl_kb_n - 1) >> 4;
    return page > last ? last : page;
}

/* The slice page texts end in "/4": on DIGISLICER they count its own
 * pages. UP/DOWN's popup and the keyboard menu's line. */
static char pfmt[] = "Slice Page: %d/4", mfmt[] = "  SLICE PAGE: %d/4";
static u32 pages_fmt(char *f, s32 len, u32 stock)
{
    if (!dsl_kb_n)
        return stock;
    f[len - 1] = '1' + ((dsl_kb_n - 1) >> 4);
    return (u32)f;
}
u32 dsl_kb_pfmt(void)
{
    return pages_fmt(pfmt, sizeof pfmt - 1, 0x401c2dbd);
}
u32 dsl_kb_mfmt(void)
{
    return pages_fmt(mfmt, sizeof mfmt - 1, 0x401cfe42);
}

/* Source 0x40 is not the keyboard's alone: a trig's preview (0x400345a0)
 * and a view that plays note 60 (0x400b344a) use it too, and so does the
 * slice editor. The keyboard's notes are NOTEON's calls from 0x40028bae,
 * in 0x40028b44. kb_notes marks, per track, the notes 12-75 the keyboard
 * started last (any other call of that note clears it), so recording
 * converts only those. */
#define KB_CALL 0x40028bb4                 /* NOTEON's return in 0x40028b44 */
static u32 kb_notes[8][2];

static s32 kb_note(s32 track, s32 note, s32 set)
{
    u32 b = note - 12, m = 1u << (b & 31), *w = &kb_notes[track][b >> 5];
    s32 was = (*w & m) != 0;
    if (set)
        *w |= m;
    else
        *w &= ~m;
    return was;
}

/* In NOTEON, where it would attach the preview step's locks to the voice
 * (fp its frame: +4 its return, +20 the source, -8 the voice's lock list,
 * empty so far; audio its "audio voice" flags): a key's note on a
 * DIGISLICER track gets a list with one lock, SLICE = 0, so its note picks
 * its slice. */
void dsl_kb_noteon(s32 track, s32 note, u8 *fp, u32 audio_a, u32 audio_b)
{
    u8 *l;
    s32 key;
    if (*(s32 *)(fp + 20) != SRC_KEYS || note < 12 || note > 75 || track < 0 || track > 7)
        return;
    key = *(u32 *)(fp + 4) == KB_CALL;
    kb_note(track, note, key);
    if (!key || PREVIEW <= 63 || *(u32 *)(fp - 8) || !((audio_a | audio_b) & 0xff)
        || !kb_slices(track))
        return;
    l = (u8 *)LOCKLIST();
    *(s32 *)(l + 8) = 1;                   /* the number of locks */
    *(u16 *)(l + 0x14) = P_SLICE;          /* lock 0: its slot, its value */
    *(s16 *)(l + 0x16) = 0;
    *(u32 *)(fp - 8) = (u32)l;
}

/* A note message being recorded (+12 its source): 1 if the keyboard started
 * this note on a track in trig slice mode. Its mark is used up. */
static s32 kb_recorded(const u8 *msg, s32 track, s32 note)
{
    return *(const s32 *)(msg + 12) == SRC_KEYS && note >= 12 && note <= 75
        && track >= 0 && track <= 7 && kb_note(track, note, 0) && kb_slices(track);
}

/* After live recording has recorded a note (msg: the UI's note message, +8
 * the track, +16 the note, +0x1C the step): a key's note on a DIGISLICER
 * track, recorded as NOTE = 12 + slice, becomes the slice's SLICE lock with
 * the track's NOTE. The pattern track's data has the trigs' NOTEs at
 * +0x280. */
void dsl_kb_rec(const u8 *msg)
{
    s32 track = (s8)msg[8], note = (s8)msg[16], step = *(const s16 *)(msg + 0x1c);
    u32 trk;
    const s8 *d;
    if (!kb_recorded(msg, track, note))
        return;
    if (step < 0)                          /* as 0x4001c10a clamps it */
        step = 0;
    else if (step > 63)
        step = 63;
    trk = PTRACK(PATTERN(APP()), track);
    d = (const s8 *)odata(trk);
    if (!d || d[0x280 + step] != note)
        return;                            /* not recorded there */
    LOCKSET(trk, step, P_SLICE, (note - 11) << 8);
    NOTESET(trk, step, -1);
}

/* STEP REC (FUNC + a trig key, the keyboard on) records a note at the
 * cursor with 0x400088b6(ctx, step, vel): ctx[0] points to the track,
 * ctx[1] is the note message. Its NOTE = note (NOTESET(trk, step, note))
 * comes here: a key's note on a DIGISLICER track becomes the slice's SLICE
 * lock with the track's NOTE, as in live recording. */
s32 dsl_kb_stepnote(u32 trk, s32 step, s32 note, const u32 *ctx)
{
    if (kb_recorded((const u8 *)ctx[1], *(const s32 *)ctx[0], note)) {
        LOCKSET(trk, step, P_SLICE, (note - 11) << 8);
        note = -1;
    }
    return NOTESET(trk, step, note);
}
